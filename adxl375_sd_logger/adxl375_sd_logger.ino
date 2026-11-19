// =====================================================================================
// ADXL375 -> SD card CSV logger for ESP32-DevKitC (Arduino framework)
// + DS1302 RTC (on its own pins), SSD1306 OLED, zero button.
//
// What it does:
//   1. On boot, reads the date/time from the DS1302 real-time clock.
//   2. Sets up the ADXL375 accelerometer (+-200 g) at 1600 samples per second.
//   3. Creates a new CSV file on the SD card and writes one line per sample.
//   4. Shows the date, clock and live X/Y/Z values on the OLED.
//   5. The zero button (GPIO32, e.g. base board KEY1) TOGGLES the zero: first press averages
//      100 ms of Z readings and uses them as the new zero for Z ONLY (X and Y are always raw),
//      so Z reads ~0 g instead of ~1 g of gravity. Next press goes back
//      to raw values, and so on. The zero is NOT remembered: every reset starts raw.
//   6. At boot, if WIFI_SSID is filled in, joins WiFi for up to ~15 s, gets the time from NTP
//      and writes it to the RTC on the exact second, then turns WiFi OFF before logging starts.
//      No WiFi (or SSID left empty): it carries on with the RTC as below.
//   7. Every NEW upload sets the RTC to the compile time (only if NTP failed). For an exact second, type the time
//      in Serial Monitor (115200, line ending "Newline") as  YYYY-MM-DD HH:MM:SS  and press
//      Enter on that second; the RTC is set and the ESP32 restarts with a new log file.
//
// Libraries (Library Manager): "Adafruit SSD1306" (pulls in Adafruit GFX + BusIO).
// Everything else ships with the ESP32 board package.
//
// Each boot creates a new file named from the RTC, e.g. /20261118_093000.csv
// (falls back to /log_000.csv ... if the RTC is missing).
// Columns: date, time (HH:MM:SS), us (microseconds within that second), x_g, y_g, z_g
// =====================================================================================

#include <SPI.h>               // SPI bus (accelerometer and SD card)
#include <SD.h>                // FAT32 file system on the SD card
#include <Wire.h>              // I2C bus (OLED)
#include <Adafruit_GFX.h>      // text drawing for the OLED
#include <Adafruit_SSD1306.h>  // OLED driver
#include <Preferences.h>       // small store in flash: remembers which build last set the RTC
#include <time.h>              // struct tm, mktime, gmtime_r
#include "esp_timer.h"         // 64-bit microsecond timer (does not wrap like micros())
#include <WiFi.h>              // only used at boot, to get the time from NTP
#include <sys/time.h>          // gettimeofday: NTP time with microseconds

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
// DS1302 RTC on its own 3 pins (no longer shared with the SD card).
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
const uint32_t SAMPLE_INTERVAL_US = 625;   // 1600 Hz, matches the sensor's output data rate
const uint32_t FLUSH_INTERVAL_MS  = 1000;  // max data lost on power cut
const uint32_t PRINT_INTERVAL_MS  = 500;   // Serial preview rate
const float    G_PER_LSB          = 0.049; // ADXL375 datasheet: 49 mg/LSB
const int      ZERO_SAMPLES       = 160;   // 100 ms average when the zero button is pressed

// ---- WiFi / NTP time sync at boot ----
// Fill in your WiFi to set the RTC from the internet at every boot. Leave "" to skip.
// Do NOT push your real password to GitHub.
const char    *WIFI_SSID          = "";
const char    *WIFI_PASS          = "";
const long     TZ_OFFSET_S        = 8 * 3600; // Singapore is UTC+8; the RTC and log use local time

// ---- ADXL375 registers (addresses from the datasheet) ----
const uint8_t REG_DEVID       = 0x00;  // reads 0xE5
const uint8_t REG_BW_RATE     = 0x2C;  // output data rate
const uint8_t REG_POWER_CTL   = 0x2D;  // standby / measure
const uint8_t REG_DATA_FORMAT = 0x31;  // SPI mode and data layout
const uint8_t REG_DATAX0      = 0x32;  // first of 6 data bytes: X lo, X hi, Y lo, Y hi, Z lo, Z hi

// ---- Hardware objects ----
SPIClass adxlSPI(HSPI);                                        // second SPI bus, for the ADXL375
const SPISettings adxlSettings(5000000, MSBFIRST, SPI_MODE3); // 5 MHz max, mode 3
Adafruit_SSD1306 oled(128, 64, &Wire, -1);                    // 128x64 OLED, no reset pin
Preferences prefs;

// ---- Logging state ----
File logFile;
char logPath[24];                                // e.g. "/20261118_093000.csv"
uint32_t nextSampleUs, lastFlushMs, lastPrintMs, sampleCount;
int64_t bootEpoch, bootTimerUs;  // RTC time at boot + matching esp_timer value

// ---- Zero (offset) state ----
float offZ;                      // Z zero offset in raw counts (X/Y are never offset), back to 0 on every reset
bool zeroed = false;             // toggled by the zero button; false = raw values
int zeroN = -1;                  // >= 0 while averaging for a zero
int32_t sumZ;                    // running Z sum while averaging

// Latest values for the display task (core 0); written by loop() (core 1)
volatile float dispX, dispY, dispZ;
bool oledOk;                     // false if no OLED answered at boot

// ================= ADXL375 =================
// Write one register. First byte is the address (bit 7 = 0 means write).
void adxlWrite(uint8_t reg, uint8_t val) {
  adxlSPI.beginTransaction(adxlSettings);
  digitalWrite(ADXL_CS, LOW);
  adxlSPI.transfer(reg & 0x3F);
  adxlSPI.transfer(val);
  digitalWrite(ADXL_CS, HIGH);
  adxlSPI.endTransaction();
}

// Read len registers starting at reg into buf. Reading all 6 data bytes in one go
// guarantees X, Y and Z come from the same sample.
void adxlRead(uint8_t reg, uint8_t *buf, uint8_t len) {
  adxlSPI.beginTransaction(adxlSettings);
  digitalWrite(ADXL_CS, LOW);
  adxlSPI.transfer(0x80 | (len > 1 ? 0x40 : 0x00) | (reg & 0x3F)); // read bit + multi-byte bit
  for (uint8_t i = 0; i < len; i++) buf[i] = adxlSPI.transfer(0x00);
  digitalWrite(ADXL_CS, HIGH);
  adxlSPI.endTransaction();
}

// ================= DS1302 (3-wire, LSB first) =================
// The pins are bit-banged by hand because the DS1302 is not real SPI (one shared data line, LSB first).

// Send one byte, lowest bit first. If thenRead is true, the RTC will answer right after,
// so the data pin is switched to input before the last clock falls.
void rtcWriteByte(uint8_t b, bool thenRead = false) {
  pinMode(RTC_IO, OUTPUT);
  for (int i = 0; i < 8; i++) {
    digitalWrite(RTC_IO, b & 1);
    b >>= 1;
    delayMicroseconds(1);
    digitalWrite(RTC_CLK, HIGH);   // RTC latches the bit on the rising edge
    delayMicroseconds(1);
    if (thenRead && i == 7) pinMode(RTC_IO, INPUT);  // release I/O before the RTC drives it
    digitalWrite(RTC_CLK, LOW);
  }
}

// Receive one byte, lowest bit first. The RTC puts each bit out on the falling clock edge.
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

// Start (on = true) or end (on = false) a transfer. Clock must be low when CE changes.
void rtcSelect(bool on) {
  digitalWrite(RTC_CLK, LOW);
  digitalWrite(RTC_CE, on);
  delayMicroseconds(4);
}

// The DS1302 stores numbers as BCD: 0x59 means 59.
uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Wait (up to 1.1 s) for the RTC's seconds to tick over, so the next read lands right at the
// start of a second. Without this the boot time could be up to 1 s off.
void rtcWaitForTick() {
  auto sec = []() { rtcSelect(true); rtcWriteByte(0x81, true); uint8_t v = rtcReadByte(); rtcSelect(false); return v; };
  uint8_t s0 = sec();
  uint32_t start = millis();
  while (sec() == s0 && millis() - start < 1100) {}
}

// Read date and time into t. Returns false if the clock is stopped or not connected.
bool rtcRead(struct tm &t) {
  uint8_t r[7];                             // sec, min, hour, date, month, weekday, year
  rtcSelect(true);
  rtcWriteByte(0xBF, true);                 // clock burst read
  for (int i = 0; i < 7; i++) r[i] = rtcReadByte();
  rtcSelect(false);
  if (r[0] & 0x80) return false;            // clock halted (new / battery lost)
  t = {};
  t.tm_sec  = bcd2dec(r[0] & 0x7F);
  t.tm_min  = bcd2dec(r[1] & 0x7F);
  t.tm_hour = bcd2dec(r[2] & 0x3F);          // 24 h mode (we always write 24 h)
  t.tm_mday = bcd2dec(r[3] & 0x3F);
  t.tm_mon  = bcd2dec(r[4] & 0x1F) - 1;      // struct tm months are 0-11
  t.tm_year = bcd2dec(r[6]) + 100;           // 2000-2099
  // All-0xFF / garbage (RTC not connected) fails these checks
  return t.tm_sec < 60 && t.tm_min < 60 && t.tm_hour < 24 &&
         t.tm_mday >= 1 && t.tm_mday <= 31 && t.tm_mon >= 0 && t.tm_mon < 12;
}

// Set the RTC to t and start it running.
void rtcWrite(const struct tm &t) {
  rtcSelect(true); rtcWriteByte(0x8E); rtcWriteByte(0x00); rtcSelect(false);  // write-protect off
  rtcSelect(true);
  rtcWriteByte(0xBE);                       // clock burst write (all 8 bytes)
  rtcWriteByte(dec2bcd(t.tm_sec));          // bit7 = 0 -> clock runs
  rtcWriteByte(dec2bcd(t.tm_min));
  rtcWriteByte(dec2bcd(t.tm_hour));
  rtcWriteByte(dec2bcd(t.tm_mday));
  rtcWriteByte(dec2bcd(t.tm_mon + 1));
  rtcWriteByte(dec2bcd(t.tm_wday + 1));
  rtcWriteByte(dec2bcd(t.tm_year - 100));
  rtcWriteByte(0x80);                       // control: write-protect ON, so power-up/down glitches can't change the time
  rtcSelect(false);
}

// Show a short status line on the OLED (only used during boot).
void oledMsg(const char *msg) {
  if (!oledOk) return;
  oled.clearDisplay(); oled.setTextSize(1); oled.setCursor(0, 0);
  oled.println(msg); oled.display();
}

// Join WiFi, get the time from NTP and write it to the RTC exactly at the start of a second.
// WiFi is switched off afterwards so it can't disturb 1600 Hz sampling (or drain power).
// Returns false (and leaves the RTC alone) if the SSID is empty, WiFi or NTP fails.
bool ntpSync() {
  if (WIFI_SSID[0] == 0) return false;
  Serial.printf("WiFi: connecting to %s...\n", WIFI_SSID);
  oledMsg("WiFi: syncing time...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(200);
  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");   // UTC; TZ_OFFSET_S is added below
    start = millis();
    while (time(nullptr) < 1700000000 && millis() - start < 8000) delay(100);  // wait for the answer
    if (time(nullptr) >= 1700000000) {
      struct timeval tv;
      gettimeofday(&tv, nullptr);
      time_t next = tv.tv_sec + 1 + TZ_OFFSET_S;            // the next whole second, local time
      delayMicroseconds(1000000 - tv.tv_usec);              // wait until it starts...
      struct tm t;
      gmtime_r(&next, &t);
      rtcWrite(t);                                          // ...and set the RTC right on it
      Serial.printf("NTP: RTC set to %04d-%02d-%02d %02d:%02d:%02d\n", t.tm_year + 1900, t.tm_mon + 1,
                    t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
      ok = true;
    } else {
      Serial.println("NTP: no answer, keeping the RTC time.");
    }
  } else {
    Serial.println("WiFi: not connected, keeping the RTC time.");
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return ok;
}

// The time this sketch was compiled (from the __DATE__ and __TIME__ macros), used to
// set the RTC after each new upload. It is your PC's local time, so the RTC ends up in your local time too.
struct tm compileTime() {
  struct tm t = {};
  char mon[4];
  sscanf(__DATE__, "%3s %d %d", mon, &t.tm_mday, &t.tm_year);   // e.g. "Nov 18 2026"
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec); // e.g. "09:30:00"
  // Month name -> 0-11 by its position in this string
  t.tm_mon = (strstr("JanFebMarAprMayJunJulAugSepOctNovDec", mon) - "JanFebMarAprMayJunJulAugSepOctNovDec") / 3;
  t.tm_year -= 1900;
  mktime(&t);  // fills in tm_wday
  return t;
}

// ================= Display (runs on core 0 so I2C never stalls sampling) =================
// Redraws the OLED 5 times a second:
//   line 1: clock + "ZEROING" (averaging), "ZEROED" (zero on) or "RAW" (zero off)
//   line 2: date
//   lines 3-5 (big text): X, Y, Z in g
void displayTask(void *) {
  for (;;) {
    // Current time = RTC time at boot + time elapsed since boot
    int64_t nowS = bootEpoch + (esp_timer_get_time() - bootTimerUs) / 1000000;
    time_t tt = (time_t)nowS;
    struct tm t;
    gmtime_r(&tt, &t);

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.printf("%02d:%02d:%02d  %s\n", t.tm_hour, t.tm_min, t.tm_sec,
                zeroN >= 0 ? "ZEROING" : zeroed ? "ZEROED" : "RAW");
    oled.printf("%04d-%02d-%02d\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    oled.setTextSize(2);
    oled.printf("X%7.2f\nY%7.2f\nZ%7.2f", dispX, dispY, dispZ);
    oled.display();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Fatal error: show the message on Serial and the OLED, then stop forever.
void halt(const char *msg) {
  Serial.println(msg);
  if (oledOk) {
    oled.clearDisplay(); oled.setTextSize(1); oled.setCursor(0, 0);
    oled.println(msg); oled.display();
  }
  while (true) delay(1000);
}

// ================= setup(): runs once at power-on =================
// OLED first, so errors can be shown on it.
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nADXL375 SD logger");

  pinMode(ZERO_BTN, BTN_PRESSED == LOW ? INPUT_PULLUP : INPUT_PULLDOWN);  // holds the pin at "not pressed"

  // OLED first, so later errors can be shown on it. Logging still works without it.
  Wire.begin(OLED_SDA, OLED_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) { oled.setTextColor(SSD1306_WHITE); oled.clearDisplay(); oled.display(); }
  else Serial.println("WARNING: OLED not found, continuing without display.");

  // RTC pins (CE low = RTC ignores the bus)
  pinMode(SD_CS, OUTPUT);  digitalWrite(SD_CS, HIGH);   // keep the SD card deselected
  pinMode(RTC_CE, OUTPUT); digitalWrite(RTC_CE, LOW);
  pinMode(RTC_CLK, OUTPUT); digitalWrite(RTC_CLK, LOW);
  bool ntpOk = ntpSync();   // WiFi -> NTP -> RTC; WiFi is off again afterwards
  struct tm now;
  rtcWaitForTick();
  bool rtcOk = rtcRead(now);
  // Is this the first boot of a new upload? Compare this build's compile stamp with the one
  // saved in flash. Plain resets (RST, power) keep the RTC time; only a new upload sets it.
  const char *thisBuild = __DATE__ " " __TIME__;
  char lastBuild[24] = "";
  prefs.begin("rtc", false);
  prefs.getString("build", lastBuild, sizeof(lastBuild));
  bool newBuild = strcmp(lastBuild, thisBuild) != 0;
  if (ntpOk) {
    prefs.putString("build", thisBuild);   // NTP already set it; don't overwrite with compile time
  } else if (!rtcOk || newBuild) {
    // No NTP, and a new upload / new RTC / flat battery: set it to the compile time, then read it back
    Serial.println(newBuild ? "RTC: new upload, setting to compile time" : "RTC: not running, setting to compile time");
    rtcWrite(compileTime());
    prefs.putString("build", thisBuild);
    rtcOk = rtcRead(now);
  }
  // Remember the RTC time and the ESP32 timer at the same moment.
  // Every timestamp after this = bootEpoch + (timer now - bootTimerUs), with microsecond resolution.
  bootTimerUs = esp_timer_get_time();
  if (rtcOk) {
    bootEpoch = mktime(&now);   // ESP32 default TZ is UTC, so this is the RTC's own time as epoch
    Serial.printf("RTC: %04d-%02d-%02d %02d:%02d:%02d\n", now.tm_year + 1900, now.tm_mon + 1,
                  now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
  } else {
    bootEpoch = 0;
    Serial.println("WARNING: RTC not found, timestamps are seconds since boot.");
  }

  // ADXL375
  pinMode(ADXL_CS, OUTPUT);
  digitalWrite(ADXL_CS, HIGH);  // deselected until we talk to it
  adxlSPI.begin(ADXL_SCK, ADXL_MISO, ADXL_MOSI, -1);

  // Check the sensor answers with its fixed ID before going on
  uint8_t id;
  adxlRead(REG_DEVID, &id, 1);
  Serial.printf("ADXL375 DEVID: 0x%02X\n", id);
  if (id != 0xE5) halt("ERROR: ADXL375 not found. Check wiring (CS/SCK/SDO/SDA) and 3.3V.");

  adxlWrite(REG_DATA_FORMAT, 0x0B); // 4-wire SPI, right-justified, required bits set
  adxlWrite(REG_BW_RATE, 0x0E);     // 1600 Hz output data rate
  adxlWrite(REG_POWER_CTL, 0x08);   // measurement mode


  // SD card on VSPI
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
  if (!SD.begin(SD_CS, SPI)) halt("ERROR: SD card mount failed. Check card (FAT32) and wiring.");

  // Pick the file name: date/time if the RTC works, otherwise the first free log_NNN.csv
  if (rtcOk) {
    snprintf(logPath, sizeof(logPath), "/%04d%02d%02d_%02d%02d%02d.csv", now.tm_year + 1900,
             now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
  } else {
    for (int i = 0; i < 1000; i++) {
      snprintf(logPath, sizeof(logPath), "/log_%03d.csv", i);
      if (!SD.exists(logPath)) break;
    }
  }
  logFile = SD.open(logPath, FILE_WRITE);
  if (!logFile) halt("ERROR: could not create log file.");
  logFile.println("date,time,us,x_g,y_g,z_g");  // CSV header
  logFile.flush();
  Serial.printf("Logging to %s\n", logPath);

  // Start the OLED refresh on core 0; loop() keeps core 1 for sampling
  if (oledOk) xTaskCreatePinnedToCore(displayTask, "oled", 4096, nullptr, 1, nullptr, 0);

  nextSampleUs = micros();
  lastFlushMs = lastPrintMs = millis();
}

// Set the RTC from a line typed in Serial Monitor: "YYYY-MM-DD HH:MM:SS".
// Write the RTC, then restart: the new boot reads the fresh time and starts a new file.
void checkSerialTimeSet() {
  static char line[24];
  static uint8_t len;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { if (len < sizeof(line) - 1) line[len++] = c; continue; }
    if (len == 0) continue;               // ignore the extra \r or \n of a CR+LF line ending
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
    mktime(&t);                           // fills in tm_wday
    logFile.close();                      // close the file cleanly before the restart
    rtcWrite(t);
    Serial.printf("RTC set to %s. Restarting...\n", line);
    delay(100);
    ESP.restart();
  }
}

// ================= loop(): one sample per call, every 625 us =================
void loop() {
  if ((int32_t)(micros() - nextSampleUs) < 0) return;  // not time yet
  int64_t tUs = esp_timer_get_time();                   // timestamp for this sample
  uint32_t t = micros();
  nextSampleUs += SAMPLE_INTERVAL_US;
  // If an SD write stalled us, skip ahead instead of bursting stale samples. The gap shows in the time/us columns.
  if ((int32_t)(t - nextSampleUs) > (int32_t)SAMPLE_INTERVAL_US) nextSampleUs = t + SAMPLE_INTERVAL_US;

  // Read X, Y, Z: each is 2 bytes, low byte first, signed
  uint8_t b[6];
  adxlRead(REG_DATAX0, b, 6);
  int16_t rx = b[1] << 8 | b[0];
  int16_t ry = b[3] << 8 | b[2];
  int16_t rz = b[5] << 8 | b[4];

  // Zero button toggles: raw -> zeroed (average the next ZERO_SAMPLES readings and use them
  // as the new zero) -> raw again, and so on.
  uint32_t now = millis();
  static int lastBtn = !BTN_PRESSED;
  static uint32_t lastBtnMs;
  int btn = digitalRead(ZERO_BTN);
  if (btn != lastBtn && now - lastBtnMs > 50) {   // 50 ms debounce
    lastBtn = btn;
    lastBtnMs = now;
    if (btn == BTN_PRESSED && zeroN < 0) {
      if (zeroed) {                               // zero on -> off: back to raw values
        zeroed = false;
        offZ = 0;
        Serial.println("Zero OFF (raw values).");
      } else {
        zeroN = 0; sumZ = 0;                      // zero off -> on: start averaging
      }
    }
  }
  if (zeroN >= 0) {
    sumZ += rz;
    if (++zeroN == ZERO_SAMPLES) {
      offZ = sumZ / (float)ZERO_SAMPLES;
      Serial.printf("Zero ON. Z offset: %.1f counts (%.2f g)\n", offZ, offZ * G_PER_LSB);
      zeroed = true;
      zeroN = -1;                                 // done
    }
  }

  // Raw counts -> g, after removing the zero offset
  float x = rx * G_PER_LSB;                       // X and Y are always raw
  float y = ry * G_PER_LSB;
  float z = (rz - offZ) * G_PER_LSB;
  dispX = x; dispY = y; dispZ = z;                // hand the latest values to the OLED task

  // Write one CSV line: "YYYY-MM-DD,HH:MM:SS,microseconds,x,y,z"
  // Date and time are separate columns so Excel keeps the seconds; us is the position within that second.
  int64_t e = bootEpoch * 1000000LL + (tUs - bootTimerUs);
  time_t sec = (time_t)(e / 1000000);
  static time_t lastSec = -1;
  static char stamp[24];                          // "YYYY-MM-DD,HH:MM:SS", rebuilt once a second
  if (sec != lastSec) {
    struct tm tm;
    gmtime_r(&sec, &tm);
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d,%02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    lastSec = sec;
  }
  logFile.printf("%s,%06ld,%.3f,%.3f,%.3f\n", stamp, (long)(e % 1000000), x, y, z);
  sampleCount++;

  // Push buffered data to the card once a second, so a power cut loses at most ~1 s
  if (now - lastFlushMs >= FLUSH_INTERVAL_MS) {
    logFile.flush();
    lastFlushMs = now;
  }
  // Serial preview twice a second (printing every sample would be far too slow)
  if (now - lastPrintMs >= PRINT_INTERVAL_MS) {
    Serial.printf("X=%7.2f g  Y=%7.2f g  Z=%7.2f g  samples=%lu\n", x, y, z, (unsigned long)sampleCount);
    lastPrintMs = now;
  }

  checkSerialTimeSet();                           // set the RTC from a typed time, then restart
}
