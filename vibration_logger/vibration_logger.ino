// =====================================================================================
// Vibration logger for ESP32 (Arduino): ADXL375 -> SD card CSV, DS1302 RTC, SSD1306 OLED.
//
//   * ADXL375 sampled at 1600 Hz through its 32-sample FIFO (core 1).
//   * Samples wait in a RAM buffer and are written to the SD card by a task on core 0,
//     so a slow card never stops sampling.
//   * Logging only runs while ZEROED. Power-on / RST starts RAW: the OLED shows live data
//     but nothing is written. KEY1 (GPIO32) press: average 1600 samples (1 s) of X, Y, Z,
//     subtract them, open a new CSV file and log. Next press: close the file, back to RAW.
//     Each press into ZEROED repeats this with a fresh average and a new file.
//   * Files are named from the RTC (the date is only in the name), e.g. /20261120_104205.csv.
//     One file per ZEROED session. Only if it nears FAT32's 4 GB limit (~17 h) does a new
//     file start, so data is never lost to the limit.
//     Columns: time (hh:mm:ss), us (0-999999 within that second), x_g, y_g, z_g (3 decimals)
//   * OLED every 0.5 s: date, time, RAW / ZEROING / LOGGING, then the latest X, Y, Z.
//     Errors replace X/Y/Z so nobody thinks it is logging when it is not:
//       ADXL375 not found  -> nothing logged, retried every 1 s, new file when it is back
//       NO SD CARD         -> retried every 2 s, logging starts when a card is inserted
//       SD ERROR           -> retried every 2 s, same file continued
//       SD FULL            -> logging stops until a new card + RST
//   * Lost samples ("SD gap N samples", "dropped N samples") are reported on Serial, not in the CSV.
//   * Every new upload sets the RTC to the compile time. For the exact second send
//     "YYYY-MM-DD HH:MM:SS" on Serial (115200, Newline), or use the RTC Sync app.
//     RTC problems and a missing OLED are reported on Serial only.
//
// Library (Library Manager): "Adafruit SSD1306" (pulls in Adafruit GFX + BusIO).
// Everything else ships with the ESP32 board package. Board: "ESP32 Dev Module".
// =====================================================================================

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <time.h>
#include "esp_timer.h"

// ---- Pins: for the Prechin ESP32 base board (checked against its schematic). ----
// Avoid strapping pins (0, 2, 5, 12, 15) and input-only pins (34-39) for outputs.
// ADXL375 (HSPI bus, its own SPI bus so SD writes never block sensor reads)
const int ADXL_SCK  = 14;
const int ADXL_MISO = 27;   // ADXL375 SDO
const int ADXL_MOSI = 13;   // ADXL375 SDA/SDI
const int ADXL_CS   = 26;
// SD card (VSPI bus)
const int SD_SCK    = 18;
const int SD_MISO   = 19;
const int SD_MOSI   = 23;
const int SD_CS     = 4;    // base board TF slot is hardwired to GPIO4
// DS1302 RTC (bit-banged 3-wire).
// 16/17 only go to the base board's servo headers and LCD socket: leave those empty.
const int RTC_CLK   = 16;   // DS1302 CLK (SCLK)
const int RTC_IO    = 17;   // DS1302 DAT (I/O)
const int RTC_CE    = 25;   // DS1302 RST (CE)
// SSD1306 OLED (I2C): wire with jumper leads, NOT the board's OLED socket (that uses SD's 18/23)
const int OLED_SDA  = 21;
const int OLED_SCL  = 22;
const uint8_t OLED_ADDR = 0x3C;  // some modules use 0x3D
// Zero button on GPIO32. GPIO32 has internal pull-up AND pull-down resistors,
// so no external resistor is needed. The sketch picks the right one from BTN_PRESSED:
//   LOW  (default): button between GPIO32 and GND, e.g. base board KEY1. Internal pull-up.
//   HIGH          : button between GPIO32 and 3V3. Internal pull-down.
const int ZERO_BTN  = 32;
const int BTN_PRESSED = LOW;
// ---- end of pins ----

// ---- Settings ----
const uint32_t SAMPLE_US        = 625;    // 1600 Hz
const uint32_t OLED_MS          = 500;    // OLED refresh (raise it if the display looks jerky)
const uint32_t FLUSH_MS         = 1000;   // a power cut loses at most ~1 s of data
const uint32_t SD_RETRY_MS      = 2000;   // how often a missing / failed card is tried again
const uint32_t ADXL_RETRY_MS    = 1000;   // how often a missing sensor is tried again
const uint32_t ADXL_CHECK_MS    = 500;    // how often a working sensor's ID is checked
const uint64_t SD_MIN_FREE      = 1048576; // below 1 MB free the card counts as full
const uint32_t MAX_FILE_BYTES   = 4000000000UL; // FAT32 can't hold a file over 4 GiB: start a new one first
const int      ZERO_SAMPLES     = 1600;   // 1 s average for the zero
const int      BUFFER_SAMPLES   = 4000;   // RAM buffer: ~2.5 s of samples if the card stalls
const char    *CSV_HEADER       = "time,us,x_g,y_g,z_g\n";

// ---- ADXL375 registers ----
const uint8_t REG_DEVID = 0x00, REG_BW_RATE = 0x2C, REG_POWER_CTL = 0x2D,
              REG_DATA_FORMAT = 0x31, REG_DATAX0 = 0x32, REG_FIFO_CTL = 0x38, REG_FIFO_STATUS = 0x39;

SPIClass adxlSPI(HSPI);
const SPISettings adxlSettings(5000000, MSBFIRST, SPI_MODE3);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Preferences prefs;

// One sample in raw counts (49 mg each), zero already removed.
// drop = samples lost just before this one because the RAM buffer was full.
struct Sample { int64_t tUs; int16_t x, y, z; uint32_t drop; };
QueueHandle_t sampleQ;

enum { SD_NONE, SD_OK, SD_FAIL, SD_FULL };
volatile uint8_t sdState = SD_NONE;       // set by sdTask, shown by the OLED
volatile bool adxlOk = false;             // set by loop(), read by sdTask and the OLED
volatile bool logging = false;            // true while ZEROED: loop() queues samples, sdTask writes them
volatile bool stopLogging = false, logClosed = false;  // clean close before a restart
volatile uint32_t lostSamples, fifoFull, sampleCount;   // Serial counters
int64_t bootEpoch, bootTimerUs;           // RTC time at boot + matching esp_timer value

// Zero
bool zeroed = false;
int16_t offX, offY, offZ;
int zeroN = -1;                           // >= 0 while averaging
int32_t sumX, sumY, sumZ;

// Latest sample for the OLED, always copied as one set under dispMux
portMUX_TYPE dispMux = portMUX_INITIALIZER_UNLOCKED;
int16_t dispX, dispY, dispZ;
uint8_t dispMode;                         // 0 RAW, 1 ZEROING, 2 LOGGING
bool oledOk;

// ================= ADXL375 =================
void adxlWrite(uint8_t reg, uint8_t val) {
  adxlSPI.beginTransaction(adxlSettings);
  digitalWrite(ADXL_CS, LOW);
  adxlSPI.transfer(reg & 0x3F);
  adxlSPI.transfer(val);
  digitalWrite(ADXL_CS, HIGH);
  adxlSPI.endTransaction();
}

void adxlRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  adxlSPI.beginTransaction(adxlSettings);
  digitalWrite(ADXL_CS, LOW);
  adxlSPI.transfer(0x80 | (len > 1 ? 0x40 : 0x00) | (reg & 0x3F));  // read + multi-byte bits
  for (uint8_t i = 0; i < len; i++) buf[i] = adxlSPI.transfer(0x00);
  digitalWrite(ADXL_CS, HIGH);
  adxlSPI.endTransaction();
}

bool adxlPresent() { uint8_t id; adxlRead(REG_DEVID, &id, 1); return id == 0xE5; }

// Check the ID and (re)configure. Also used after the sensor comes back from a fault.
bool adxlInit() {
  if (!adxlPresent()) return false;
  adxlWrite(REG_POWER_CTL, 0x00);    // standby while configuring
  adxlWrite(REG_DATA_FORMAT, 0x0B);  // 4-wire SPI, right-justified
  adxlWrite(REG_BW_RATE, 0x0E);      // 1600 Hz
  adxlWrite(REG_FIFO_CTL, 0x00);     // bypass first: clears old FIFO contents
  adxlWrite(REG_FIFO_CTL, 0x80);     // stream mode: keeps the newest 32 samples
  adxlWrite(REG_POWER_CTL, 0x08);    // measure
  return true;
}

// ================= DS1302 (3-wire, LSB first, bit-banged) =================
void rtcWriteByte(uint8_t b, bool thenRead = false) {
  pinMode(RTC_IO, OUTPUT);
  for (int i = 0; i < 8; i++) {
    digitalWrite(RTC_IO, b & 1);
    b >>= 1;
    delayMicroseconds(1);
    digitalWrite(RTC_CLK, HIGH);
    delayMicroseconds(1);
    if (thenRead && i == 7) pinMode(RTC_IO, INPUT);  // release I/O before the RTC drives it
    digitalWrite(RTC_CLK, LOW);
  }
}

uint8_t rtcReadByte() {
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    delayMicroseconds(1);
    b |= digitalRead(RTC_IO) << i;
    digitalWrite(RTC_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(RTC_CLK, LOW);
  }
  return b;
}

void rtcSelect(bool on) {
  digitalWrite(RTC_CLK, LOW);
  digitalWrite(RTC_CE, on);
  delayMicroseconds(4);
}

uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Wait (max 1.1 s) for the seconds to tick, so the boot time lands on the start of a second.
void rtcWaitForTick() {
  auto sec = []() { rtcSelect(true); rtcWriteByte(0x81, true); uint8_t v = rtcReadByte(); rtcSelect(false); return v; };
  uint8_t s0 = sec();
  uint32_t start = millis();
  while (sec() == s0 && millis() - start < 1100) {}
}

// False if the clock is halted (flat / missing coin cell) or not connected.
bool rtcRead(struct tm &t) {
  uint8_t r[7];
  rtcSelect(true);
  rtcWriteByte(0xBF, true);                 // clock burst read
  for (int i = 0; i < 7; i++) r[i] = rtcReadByte();
  rtcSelect(false);
  if (r[0] & 0x80) return false;            // clock halt flag
  t = {};
  t.tm_sec  = bcd2dec(r[0] & 0x7F);
  t.tm_min  = bcd2dec(r[1] & 0x7F);
  t.tm_hour = bcd2dec(r[2] & 0x3F);
  t.tm_mday = bcd2dec(r[3] & 0x3F);
  t.tm_mon  = bcd2dec(r[4] & 0x1F) - 1;
  t.tm_year = bcd2dec(r[6]) + 100;
  return t.tm_sec < 60 && t.tm_min < 60 && t.tm_hour < 24 && t.tm_mday >= 1 &&
         t.tm_mday <= 31 && t.tm_mon >= 0 && t.tm_mon < 12 && t.tm_year >= 125;  // year >= 2025
}

void rtcWrite(const struct tm &t) {
  rtcSelect(true); rtcWriteByte(0x8E); rtcWriteByte(0x00); rtcSelect(false);  // write-protect off
  rtcSelect(true);
  rtcWriteByte(0xBE);                       // clock burst write
  rtcWriteByte(dec2bcd(t.tm_sec));          // bit 7 = 0: clock runs
  rtcWriteByte(dec2bcd(t.tm_min));
  rtcWriteByte(dec2bcd(t.tm_hour));
  rtcWriteByte(dec2bcd(t.tm_mday));
  rtcWriteByte(dec2bcd(t.tm_mon + 1));
  rtcWriteByte(dec2bcd(t.tm_wday + 1));
  rtcWriteByte(dec2bcd(t.tm_year - 100));
  rtcWriteByte(0x00);
  rtcSelect(false);
}

// PC local time when this sketch was compiled.
struct tm compileTime() {
  struct tm t = {};
  char mon[4];
  sscanf(__DATE__, "%3s %d %d", mon, &t.tm_mday, &t.tm_year);
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  t.tm_mon = (strstr(months, mon) - months) / 3;
  t.tm_year -= 1900;
  mktime(&t);  // fills tm_wday
  return t;
}

// ================= CSV text =================
int64_t epochUs(int64_t tUs) { return bootEpoch * 1000000LL + (tUs - bootTimerUs); }

// "hh:mm:ss,uuuuuu," (time text is rebuilt only when the second changes)
char *putTime(char *p, int64_t eUs) {
  static time_t lastSec = -1;
  static char stamp[20];
  time_t sec = eUs / 1000000;
  if (sec != lastSec) {
    struct tm tm;
    gmtime_r(&sec, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    lastSec = sec;
  }
  return p + sprintf(p, "%s,%06ld,", stamp, (long)(eUs % 1000000));
}

// Counts -> g with 3 decimals. 1 count = 49 mg exactly, so integer maths gives the exact value.
char *putG(char *p, int counts) {
  int mg = counts * 49;
  if (mg < 0) { *p++ = '-'; mg = -mg; }
  return p + sprintf(p, "%d.%03d", mg / 1000, mg % 1000);
}

// ================= SD writer (core 0) =================
File logFile;
char logPath[32];
static char wbuf[4096];        // CSV text waiting to go to the card
static size_t wlen;
static uint32_t wrows;         // sample rows in wbuf (lost if the card fails)
static int64_t wfirstUs;       // time of the first of them
static bool mounted;           // card mounted and not full
static bool resume;            // after an SD error: continue the same file
static uint32_t fileBytes;     // size of the open file
static uint32_t gapN;          // samples lost while the card was unusable
static int64_t gapUs;          // time of the first of them

void loseRows(uint32_t n, int64_t eUs) { if (!gapN) gapUs = eUs; gapN += n; }

bool writeOut() {
  if (!wlen) return true;
  bool ok = logFile.write((const uint8_t *)wbuf, wlen) == wlen;
  fileBytes += wlen;
  wlen = 0; wrows = 0;
  return ok;
}

// The card stopped taking data: count what was in RAM as lost and unmount. The retry in
// sdTask remounts it, and decides whether it is full.
void sdFailed() {
  if (wrows) loseRows(wrows, wfirstUs);
  wlen = wrows = 0;
  logFile.close();
  SD.end();
  mounted = false;
  resume = true;
  sdState = SD_FAIL;
  Serial.println("SD ERROR: write failed, retrying");
}

// Open the same file again after an SD error, else a new one named from eUs.
bool openLog(int64_t eUs) {
  time_t sec = eUs / 1000000;
  if (resume) {
    logFile = SD.open(logPath, FILE_APPEND);
    if (logFile) fileBytes = logFile.size();
  } else {
    struct tm tm;
    gmtime_r(&sec, &tm);
    char base[20];
    strftime(base, sizeof(base), "/%Y%m%d_%H%M%S", &tm);
    snprintf(logPath, sizeof(logPath), "%s.csv", base);
    // FILE_WRITE would wipe an existing file (e.g. RTC went back in time): pick a free name
    for (int i = 1; SD.exists(logPath) && i < 100; i++) snprintf(logPath, sizeof(logPath), "%s_%d.csv", base, i);
    logFile = SD.open(logPath, FILE_WRITE);
    if (logFile && logFile.print(CSV_HEADER) != strlen(CSV_HEADER)) logFile.close();
    fileBytes = strlen(CSV_HEADER);
  }
  resume = false;
  if (!logFile) return false;
  sdState = SD_OK;
  Serial.printf("Logging to %s\n", logPath);
  return true;
}

// Close the open file normally (back to RAW, size limit, sensor lost, restart).
void closeLog() {
  if (!logFile) return;
  if (writeOut()) logFile.close();
  else sdFailed();
  resume = false;
}

void sdTask(void *) {
  uint32_t lastTry = millis() - SD_RETRY_MS, lastFlush = millis();
  Sample s;
  for (;;) {
    bool got = xQueueReceive(sampleQ, &s, pdMS_TO_TICKS(50)) == pdTRUE;

    // Buffer empty and logging stopped (back to RAW, sensor lost or restart): close the file.
    // The next ZEROED session starts a fresh file with no leftover gap row.
    if (!got && (stopLogging || !adxlOk || !logging)) {
      closeLog();
      gapN = 0;
      resume = false;
      if (stopLogging) { logClosed = true; vTaskDelete(nullptr); }
    }

    // Mount (or remount) the card every 2 s until it works
    if (!mounted && sdState != SD_FULL && millis() - lastTry >= SD_RETRY_MS) {
      lastTry = millis();
      SD.end();
      if (SD.begin(SD_CS, SPI)) {
        if (SD.totalBytes() - SD.usedBytes() >= SD_MIN_FREE) { mounted = true; sdState = SD_OK; }
        else { SD.end(); sdState = SD_FULL; Serial.println("SD FULL: swap card, press RST"); }
      }
    }

    if (got) {
      int64_t e = epochUs(s.tUs);
      if (sdState == SD_FULL) continue;                      // nothing written until RST
      if (mounted && logFile && fileBytes > MAX_FILE_BYTES) closeLog();  // near 4 GB: new file
      if (mounted && !logFile && !openLog(e)) sdFailed();
      if (!logFile) { loseRows(1 + s.drop, e); continue; }

      char *p = wbuf + wlen;
      if (gapN) { Serial.printf("SD gap %lu samples\n", (unsigned long)gapN); gapN = 0; }
      if (s.drop) Serial.printf("dropped %lu samples\n", (unsigned long)s.drop);
      if (!wrows) wfirstUs = e;
      p = putTime(p, e);
      p = putG(p, s.x); *p++ = ',';
      p = putG(p, s.y); *p++ = ',';
      p = putG(p, s.z);
      *p++ = '\n';
      wlen = p - wbuf;
      wrows++;
      if (wlen > sizeof(wbuf) - 200 && !writeOut()) sdFailed();  // block full
    }

    if (logFile && millis() - lastFlush >= FLUSH_MS) {
      lastFlush = millis();
      if (writeOut()) logFile.flush();
      else sdFailed();
    }
  }
}

// ================= OLED (core 0, never touches the sensor bus or the SD card) =================
void displayTask(void *) {
  for (;;) {
    time_t now = bootEpoch + (esp_timer_get_time() - bootTimerUs) / 1000000;
    struct tm t;
    gmtime_r(&now, &t);
    int16_t x, y, z;
    uint8_t md;
    portENTER_CRITICAL(&dispMux);
    x = dispX; y = dispY; z = dispZ; md = dispMode;
    portEXIT_CRITICAL(&dispMux);

    // Errors, most important first. Each has a title and what the user should do.
    const char *title = nullptr, *l1 = "", *l2 = "";
    if (!adxlOk)                { title = "ADXL375"; l1 = "NOT FOUND";        l2 = "check wiring"; }
    else if (sdState == SD_NONE) { title = "NO SD CARD"; l1 = "insert FAT32 card"; }
    else if (sdState == SD_FAIL) { title = "SD ERROR";   l1 = "logging stopped";  l2 = "check card"; }
    else if (sdState == SD_FULL) { title = "SD FULL";    l1 = "swap card,";       l2 = "then press RST"; }

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.printf("%02d/%02d/%02d  %02d:%02d:%02d\n", t.tm_mday, t.tm_mon + 1, t.tm_year % 100,
                t.tm_hour, t.tm_min, t.tm_sec);
    oled.print(md == 2 ? "ZEROED - LOGGING" : md == 1 ? "ZEROING - hold still" : "RAW - not logging");
    oled.setCursor(0, 16);
    if (title) {
      oled.setTextSize(2);
      oled.println(title);
      oled.setTextSize(1);
      oled.setCursor(0, 40);
      oled.println(l1);
      oled.println(l2);
    } else {
      oled.setTextSize(2);
      oled.printf("X%8.3f\nY%8.3f\nZ%8.3f", x * 0.049f, y * 0.049f, z * 0.049f);
    }
    oled.display();
    vTaskDelay(pdMS_TO_TICKS(OLED_MS));
  }
}

// ================= setup() =================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nVibration logger");

  pinMode(ZERO_BTN, BTN_PRESSED == LOW ? INPUT_PULLUP : INPUT_PULLDOWN);

  Wire.begin(OLED_SDA, OLED_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) { oled.setTextColor(SSD1306_WHITE); oled.clearDisplay(); oled.display(); }
  else Serial.println("OLED not found, logging without display.");

  pinMode(SD_CS, OUTPUT);   digitalWrite(SD_CS, HIGH);
  pinMode(ADXL_CS, OUTPUT); digitalWrite(ADXL_CS, HIGH);
  pinMode(RTC_CE, OUTPUT);  digitalWrite(RTC_CE, LOW);
  pinMode(RTC_CLK, OUTPUT); digitalWrite(RTC_CLK, LOW);

  // RTC: a new upload (different compile stamp in flash) or a stopped clock -> compile time
  struct tm now;
  rtcWaitForTick();
  bool rtcOk = rtcRead(now);
  const char *thisBuild = __DATE__ " " __TIME__;
  char lastBuild[24] = "";
  prefs.begin("rtc", false);
  prefs.getString("build", lastBuild, sizeof(lastBuild));
  bool newBuild = strcmp(lastBuild, thisBuild) != 0;
  if (!rtcOk) Serial.println("RTC LOST (halted, flat coin cell or not connected): run RTC Sync");
  if (!rtcOk || newBuild) {
    Serial.println("RTC: setting to compile time");
    rtcWrite(compileTime());
    prefs.putString("build", thisBuild);
    rtcOk = rtcRead(now);
  }
  bootTimerUs = esp_timer_get_time();
  if (rtcOk) {
    bootEpoch = mktime(&now);  // ESP32 TZ is UTC, so this is the RTC's own time
    Serial.printf("RTC: %02d/%02d/%02d %02d:%02d:%02d\n", now.tm_mday, now.tm_mon + 1,
                  now.tm_year % 100, now.tm_hour, now.tm_min, now.tm_sec);
  } else {
    bootEpoch = 0;
    Serial.println("RTC not answering: timestamps start at 01/01/70. Check RTC wiring.");
  }

  adxlSPI.begin(ADXL_SCK, ADXL_MISO, ADXL_MOSI, -1);
  adxlOk = adxlInit();
  if (!adxlOk) Serial.println("ADXL375 NOT FOUND: check wiring (CS/SCK/SDO/SDA) and 3.3V");

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);  // sdTask mounts the card

  sampleQ = xQueueCreate(BUFFER_SAMPLES, sizeof(Sample));
  if (!sampleQ) { Serial.println("Not enough RAM: lower BUFFER_SAMPLES"); while (true) delay(1000); }
  xTaskCreatePinnedToCore(sdTask, "sd", 8192, nullptr, 2, nullptr, 0);
  if (oledOk) xTaskCreatePinnedToCore(displayTask, "oled", 4096, nullptr, 1, nullptr, 0);
}

// "YYYY-MM-DD HH:MM:SS" on Serial sets the RTC, then restarts with a new file.
void checkSerialTimeSet() {
  static char line[24];
  static uint8_t len;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { if (len < sizeof(line) - 1) line[len++] = c; continue; }
    if (len == 0) continue;
    line[len] = 0;
    len = 0;
    struct tm t = {};
    if (sscanf(line, "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) != 6 || t.tm_year < 2000 || t.tm_year > 2099 ||
        t.tm_mon < 1 || t.tm_mon > 12 || t.tm_mday < 1 || t.tm_mday > 31 ||
        t.tm_hour > 23 || t.tm_min > 59 || t.tm_sec > 59) {
      Serial.println("Time not understood. Type it as YYYY-MM-DD HH:MM:SS");
      continue;
    }
    t.tm_year -= 1900; t.tm_mon -= 1;
    mktime(&t);
    stopLogging = true;                     // sdTask writes what is left and closes the file
    uint32_t start = millis();
    while (!logClosed && millis() - start < 3000) delay(10);
    rtcWrite(t);
    Serial.printf("RTC set to %s. Restarting...\n", line);
    delay(100);
    ESP.restart();
  }
}

void sensorLost() {
  adxlOk = false;
  Serial.println("ADXL375 NOT FOUND: stopped logging, retrying every 1 s");
}

// ================= loop() (core 1): sample the sensor =================
void loop() {
  if (stopLogging) return;
  checkSerialTimeSet();
  uint32_t nowMs = millis();

  // KEY1 toggles zero: raw -> average the next 100 ms and subtract it -> raw ...
  static int lastBtn = !BTN_PRESSED;
  static uint32_t lastBtnMs;
  int btn = digitalRead(ZERO_BTN);
  if (btn != lastBtn && nowMs - lastBtnMs > 50) {   // 50 ms debounce
    lastBtn = btn;
    lastBtnMs = nowMs;
    if (btn == BTN_PRESSED && zeroN < 0) {
      if (zeroed) { zeroed = logging = false; offX = offY = offZ = 0; Serial.println("Zero OFF (raw), logging stopped"); }
      else { zeroN = 0; sumX = sumY = sumZ = 0; }
    }
  }

  // Sensor missing: retry every 1 s, sample nothing
  static uint32_t lastAdxlMs;
  if (!adxlOk) {
    if (nowMs - lastAdxlMs >= ADXL_RETRY_MS) {
      lastAdxlMs = nowMs;
      if (adxlInit()) { adxlOk = true; Serial.println("ADXL375 found, logging"); }
    }
    delay(2);
    return;
  }
  if (nowMs - lastAdxlMs >= ADXL_CHECK_MS) {
    lastAdxlMs = nowMs;
    if (!adxlPresent()) { sensorLost(); return; }
  }

  uint8_t n;
  adxlRead(REG_FIFO_STATUS, &n, 1);
  n &= 0x3F;
  if (n > 32) { sensorLost(); return; }           // impossible count: bus reads garbage
  int64_t tRead = esp_timer_get_time();
  if (n == 32) fifoFull++;                        // FIFO was full: some samples may be overwritten

  static int64_t lastTUs;
  static uint32_t pendingDrop;
  static int16_t x, y, z;
  for (uint8_t i = 0; i < n; i++) {
    delayMicroseconds(5);                         // datasheet: >= 5 us between FIFO reads
    uint8_t b[6];
    adxlRead(REG_DATAX0, b, 6);
    int16_t rx = b[1] << 8 | b[0], ry = b[3] << 8 | b[2], rz = b[5] << 8 | b[4];

    if (zeroN >= 0) {
      sumX += rx; sumY += ry; sumZ += rz;
      if (++zeroN == ZERO_SAMPLES) {
        offX = lroundf(sumX / (float)ZERO_SAMPLES);
        offY = lroundf(sumY / (float)ZERO_SAMPLES);
        offZ = lroundf(sumZ / (float)ZERO_SAMPLES);
        zeroed = logging = true;
        zeroN = -1;
        Serial.printf("Zero ON. Offsets X %.3f  Y %.3f  Z %.3f g\n", offX * 0.049f, offY * 0.049f, offZ * 0.049f);
      }
    }
    x = rx - offX; y = ry - offY; z = rz - offZ;

    // Older FIFO entries are 625 us apart, counting back from now
    int64_t tUs = tRead - (int64_t)(n - 1 - i) * SAMPLE_US;
    if (tUs <= lastTUs) tUs = lastTUs + 1;
    lastTUs = tUs;

    if (!logging) { pendingDrop = 0; continue; }  // RAW: display only, nothing logged
    Sample s = { tUs, x, y, z, pendingDrop };
    if (xQueueSend(sampleQ, &s, 0) == pdTRUE) { pendingDrop = 0; sampleCount++; }
    else { pendingDrop++; lostSamples++; }       // RAM buffer full: card too slow
  }

  portENTER_CRITICAL(&dispMux);
  dispX = x; dispY = y; dispZ = z; dispMode = zeroed ? 2 : zeroN >= 0 ? 1 : 0;
  portEXIT_CRITICAL(&dispMux);

  static uint32_t lastPrintMs;
  if (nowMs - lastPrintMs >= 500) {
    lastPrintMs = nowMs;
    static const char *sdNames[] = { "NO CARD", "OK", "ERROR", "FULL" };
    Serial.printf("X=%.3f Y=%.3f Z=%.3f g  %s  SD=%s  samples=%lu lost=%lu fifoFull=%lu\n",
                  x * 0.049f, y * 0.049f, z * 0.049f, zeroed ? "LOGGING" : zeroN >= 0 ? "ZEROING" : "RAW", sdNames[sdState],
                  (unsigned long)sampleCount, (unsigned long)lostSamples, (unsigned long)fifoFull);
  }
  delay(2);                                       // FIFO holds 20 ms, so 2 ms loses nothing
}
