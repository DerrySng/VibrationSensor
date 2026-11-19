// =====================================================================================
// DHT11 -> SD card CSV logger for ESP32-DevKitC (Arduino framework)
// "Dummy tester" build: same SD card, DS1302 RTC and OLED as the ADXL375 logger,
// but with a cheap DHT temperature/humidity sensor instead of the accelerometer.
// Use it to prove the SD + RTC + OLED + button wiring works before the real sensor goes in.
//
// What it does:
//   1. On boot, reads the date/time from the DS1302 real-time clock.
//   2. Creates a new CSV file on the SD card.
//   3. Every 2 seconds reads temperature + humidity and writes one CSV line.
//   4. Shows the time, temperature and humidity on the OLED.
//   5. The zero button (GPIO32, e.g. KEY1) TOGGLES a humidity-only zero: first press makes the humidity
//      at that moment the new 0 %, next press goes back to raw, and so on. Temperature is never changed. The zero is NOT
//      remembered: every reset (RST button or power-up) starts again from raw humidity.
//
// Libraries (Library Manager):
//   - "Adafruit SSD1306" (pulls in Adafruit GFX + BusIO)
//   - "DHT sensor library" by Adafruit (pulls in Adafruit Unified Sensor)
//
// Each boot creates a new file named from the RTC, e.g. /20261119_093000.csv
// (falls back to /log_000.csv ... if the RTC is missing).
// Columns: date, time, temp_c, humidity_pct (humidity_pct has the zero offset removed)
//
// Setting the clock to the second: open Serial Monitor (115200, line ending "Newline"),
// type the time as  YYYY-MM-DD HH:MM:SS  and press Enter exactly as your reference clock
// reaches that second. The RTC is set and the ESP32 restarts with a new log file.
// =====================================================================================

#include <SPI.h>               // SPI bus (SD card)
#include <SD.h>                // FAT32 file system on the SD card
#include <Wire.h>              // I2C bus (OLED)
#include <Adafruit_GFX.h>      // text drawing for the OLED
#include <Adafruit_SSD1306.h>  // OLED driver
#include <DHT.h>               // DHT11 / DHT22 driver
#include <Preferences.h>       // small store in flash: remembers which build last set the RTC
#include <time.h>              // struct tm, mktime, gmtime_r
#include "esp_timer.h"         // 64-bit microsecond timer (does not wrap like micros())

// ---- Pins: for the Prechin ESP32 base board (checked against its schematic). ----
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
// DHT data line. 27 is what the kit's own DHT11 example uses (free now the ADXL375 is gone).
// The base board's DHT11 socket (J4) already has a 10k pull-up; jumper its signal pin to GPIO27.
const int DHT_PIN   = 27;
// Zero button (humidity only) on GPIO32. GPIO32 has internal pull-up AND pull-down resistors,
// so no external resistor is needed. The sketch picks the right one from BTN_PRESSED:
//   LOW  (default): button between GPIO32 and GND, e.g. base board KEY1. Internal pull-up.
//   HIGH          : button between GPIO32 and 3V3. Internal pull-down.
const int ZERO_BTN  = 32;
const int BTN_PRESSED = LOW;
// ---- end of pins ----

// ---- Settings ----
#define DHT_TYPE DHT11                     // change to DHT22 if you swap in a DHT22 / AM2302
const uint32_t READ_INTERVAL_MS = 2000;    // DHT11 allows 1 read/s, DHT22 1 read per 2 s: 2 s suits both

// ---- Hardware objects ----
Adafruit_SSD1306 oled(128, 64, &Wire, -1);  // 128x64 OLED, no reset pin
DHT dht(DHT_PIN, DHT_TYPE);
Preferences prefs;

// ---- Logging state ----
File logFile;
char logPath[24];                // e.g. "/20261119_093000.csv"
int64_t bootEpoch, bootTimerUs;  // RTC time at boot + matching esp_timer value
uint32_t lastReadMs, rowCount;
bool oledOk;                     // false if no OLED answered at boot
float humOff = 0;                // humidity zero offset in % RH, back to 0 on every reset
float lastRawH = NAN;            // latest humidity straight from the sensor (before the offset)
float lastT = NAN, lastH = NAN;  // latest temperature and zeroed humidity, shown on the OLED
bool zeroed = false;             // toggled by each button press; false = raw humidity

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

// Current time as Unix seconds = RTC time at boot + seconds since boot
time_t nowEpoch() { return (time_t)(bootEpoch + (esp_timer_get_time() - bootTimerUs) / 1000000); }

// Draw one screen:
//   line 1: date
//   line 2 (big): clock
//   line 3 (big): temperature
//   line 4 (big): humidity
//   top right: "ZEROED" while the humidity zero is on
void drawOled(float tC, float hum) {
  if (!oledOk) return;
  time_t tt = nowEpoch();
  struct tm t;
  gmtime_r(&tt, &t);
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("%04d-%02d-%02d   %s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
              zeroed ? "ZEROED" : "");
  oled.setTextSize(2);
  oled.setCursor(0, 12);
  oled.printf("%02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
  if (isnan(tC)) oled.print("DHT ERROR\n");
  else           oled.printf("%5.1f C\n%5.1f %%", tC, hum);
  oled.display();
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
  Serial.println("\nDHT SD logger (dummy tester)");

  pinMode(ZERO_BTN, BTN_PRESSED == LOW ? INPUT_PULLUP : INPUT_PULLDOWN);  // holds the pin at "not pressed"
  dht.begin();

  // OLED first, so later errors can be shown on it. Logging still works without it.
  Wire.begin(OLED_SDA, OLED_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) { oled.setTextColor(SSD1306_WHITE); oled.clearDisplay(); oled.display(); }
  else Serial.println("WARNING: OLED not found, continuing without display.");

  // RTC pins (CE low = RTC ignores the bus)
  pinMode(SD_CS, OUTPUT);  digitalWrite(SD_CS, HIGH);   // keep the SD card deselected
  pinMode(RTC_CE, OUTPUT); digitalWrite(RTC_CE, LOW);
  pinMode(RTC_CLK, OUTPUT); digitalWrite(RTC_CLK, LOW);
  struct tm now;
  rtcWaitForTick();
  bool rtcOk = rtcRead(now);
  // Is this the first boot of a new upload? Compare this build's compile stamp with the one
  // saved in flash. Plain resets (RST button, power) keep the RTC time; only a new upload sets it.
  const char *thisBuild = __DATE__ " " __TIME__;
  char lastBuild[24] = "";
  prefs.begin("rtc", false);
  prefs.getString("build", lastBuild, sizeof(lastBuild));
  bool newBuild = strcmp(lastBuild, thisBuild) != 0;
  if (!rtcOk || newBuild) {
    // New upload, new RTC or flat battery: set it to the compile time, then read it back to check
    Serial.println(newBuild ? "RTC: new upload, setting to compile time" : "RTC: not running, setting to compile time");
    rtcWrite(compileTime());
    prefs.putString("build", thisBuild);
    rtcOk = rtcRead(now);
  }
  // Remember the RTC time and the ESP32 timer at the same moment; the clock counts on from here.
  bootTimerUs = esp_timer_get_time();
  if (rtcOk) {
    bootEpoch = mktime(&now);   // ESP32 default TZ is UTC, so this is the RTC's own time as epoch
    Serial.printf("RTC: %04d-%02d-%02d %02d:%02d:%02d\n", now.tm_year + 1900, now.tm_mon + 1,
                  now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
  } else {
    bootEpoch = 0;
    Serial.println("WARNING: RTC not found, timestamps are seconds since boot.");
  }

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
  logFile.println("date,time,temp_c,humidity_pct");  // CSV header
  logFile.flush();
  Serial.printf("Logging to %s\n", logPath);

  lastReadMs = millis() - READ_INTERVAL_MS;  // take the first reading straight away
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

// ================= loop(): check the button all the time, log every 2 s =================
void loop() {
  uint32_t ms = millis();
  checkSerialTimeSet();

  // Button: reads BTN_PRESSED while pressed; the internal pull resistor holds it at the other level. 50 ms debounce.
  static int lastBtn = !BTN_PRESSED;
  static uint32_t lastBtnMs;
  int btn = digitalRead(ZERO_BTN);
  if (btn != lastBtn && ms - lastBtnMs > 50) {
    lastBtn = btn;
    lastBtnMs = ms;
    // Each press toggles: raw -> zeroed (latest raw humidity becomes 0 %) -> raw again.
    // Temperature is left alone.
    if (btn == BTN_PRESSED && zeroed) {
      zeroed = false;
      humOff = 0;
      lastH = lastRawH;                         // show raw humidity straight away
      Serial.println("Humidity zero OFF (raw).");
    } else if (btn == BTN_PRESSED && !isnan(lastRawH)) {
      zeroed = true;
      humOff = lastRawH;
      lastH = 0;                                // show 0 % straight away
      Serial.printf("Humidity zero ON. Offset: %.1f %%\n", humOff);
    } else if (btn == BTN_PRESSED) {
      Serial.println("Zero ignored: no valid humidity reading yet.");
    }
  }

  // Redraw the OLED twice a second so the clock ticks every second (the DHT is only read every 2 s)
  static uint32_t lastDrawMs;
  if (ms - lastDrawMs >= 500) { lastDrawMs = ms; drawOled(lastT, lastH); }

  if (ms - lastReadMs < READ_INTERVAL_MS) return;  // not time for a reading yet
  lastReadMs = ms;

  float tC  = dht.readTemperature();  // deg C, NAN if the sensor did not answer
  float hum = dht.readHumidity();     // % RH, NAN if the sensor did not answer
  if (isnan(hum)) tC = NAN;           // treat a half-failed read as a failed read
  lastRawH = hum;
  hum -= humOff;                      // zeroed humidity (NAN stays NAN)

  // One CSV line: "YYYY-MM-DD,HH:MM:SS,temp,humidity" (separate date and time columns). A failed read logs "nan"
  // so you can still see the logger is running while the sensor is unplugged.
  time_t tt = nowEpoch();
  struct tm t;
  gmtime_r(&tt, &t);
  logFile.printf("%04d-%02d-%02d,%02d:%02d:%02d,%.1f,%.1f\n",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                 tC, hum);
  logFile.flush();                    // only one line every 2 s, so flush each time: a power cut loses nothing
  rowCount++;

  Serial.printf("%02d:%02d:%02d  T=%.1f C  H=%.1f %%  rows=%lu\n", t.tm_hour, t.tm_min, t.tm_sec,
                tC, hum, (unsigned long)rowCount);
  if (isnan(tC)) Serial.println("WARNING: DHT read failed. Check the data wire, 3V3/GND and the DHT_TYPE setting.");

  lastT = tC; lastH = hum;
}
