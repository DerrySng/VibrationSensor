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
//   5. The button (GPIO35) puts a "mark" on the next CSV line, so you can find a moment
//      in the file later (e.g. "I breathed on the sensor here").
//
// Libraries (Library Manager):
//   - "Adafruit SSD1306" (pulls in Adafruit GFX + BusIO)
//   - "DHT sensor library" by Adafruit (pulls in Adafruit Unified Sensor)
//
// Each boot creates a new file named from the RTC, e.g. /20261119_093000.csv
// (falls back to /log_000.csv ... if the RTC is missing).
// Columns: epoch_s, datetime, temp_c, humidity_pct, mark
// =====================================================================================

#include <SPI.h>               // SPI bus (SD card)
#include <SD.h>                // FAT32 file system on the SD card
#include <Wire.h>              // I2C bus (OLED)
#include <Adafruit_GFX.h>      // text drawing for the OLED
#include <Adafruit_SSD1306.h>  // OLED driver
#include <DHT.h>               // DHT11 / DHT22 driver
#include <time.h>              // struct tm, mktime, gmtime_r
#include "esp_timer.h"         // 64-bit microsecond timer (does not wrap like micros())

// ---- Pins: for the Prechin ESP32 base board (checked against its schematic). ----
// SD card (VSPI bus)
const int SD_SCK    = 18;
const int SD_MISO   = 19;
const int SD_MOSI   = 23;
const int SD_CS     = 4;    // base board TF slot is hardwired to GPIO4
// DS1302 RTC: SCLK -> SD_SCK, I/O -> SD_MOSI, CE (RST) on its own pin
const int RTC_CE    = 25;
// SSD1306 OLED (I2C): wire with jumper leads, NOT the board's OLED socket (that uses SD's 18/23)
const int OLED_SDA  = 21;
const int OLED_SCL  = 22;
const uint8_t OLED_ADDR = 0x3C;  // some modules use 0x3D
// DHT data line. 27 is what the kit's own DHT11 example uses (free now the ADXL375 is gone).
// The base board's DHT11 socket (J4) already has a 10k pull-up; jumper its signal pin to GPIO27.
const int DHT_PIN   = 27;
// Mark button on GPIO35. GPIO35 is INPUT-ONLY and has NO internal pull-up,
// so it needs an external 10k resistor from GPIO35 to 3V3. Button between GPIO35 and GND.
const int MARK_BTN  = 35;
// ---- end of pins ----

// ---- Settings ----
#define DHT_TYPE DHT11                     // change to DHT22 if you swap in a DHT22 / AM2302
const uint32_t READ_INTERVAL_MS = 2000;    // DHT11 allows 1 read/s, DHT22 1 read per 2 s: 2 s suits both
const bool     FORCE_SET_RTC    = false;   // true = set RTC to the compile time on boot (then set back to false and re-upload)

// ---- Hardware objects ----
Adafruit_SSD1306 oled(128, 64, &Wire, -1);  // 128x64 OLED, no reset pin
DHT dht(DHT_PIN, DHT_TYPE);

// ---- Logging state ----
File logFile;
char logPath[24];                // e.g. "/20261119_093000.csv"
int64_t bootEpoch, bootTimerUs;  // RTC time at boot + matching esp_timer value
uint32_t lastReadMs, rowCount;
bool oledOk;                     // false if no OLED answered at boot
bool markPending;                // true from a button press until the next CSV line is written
float lastT = NAN, lastH = NAN;  // latest reading, shown on the OLED

// ================= DS1302 (3-wire, LSB first) =================
// Only used in setup(), BEFORE the SD card takes over SCK/MOSI. With CE low the DS1302
// ignores the bus, and with SD_CS high the card ignores the RTC traffic.
// The pins are bit-banged by hand because the DS1302 is not real SPI (one shared data line, LSB first).

// Send one byte, lowest bit first. If thenRead is true, the RTC will answer right after,
// so the data pin is switched to input before the last clock falls.
void rtcWriteByte(uint8_t b, bool thenRead = false) {
  pinMode(SD_MOSI, OUTPUT);
  for (int i = 0; i < 8; i++) {
    digitalWrite(SD_MOSI, b & 1);
    b >>= 1;
    delayMicroseconds(1);
    digitalWrite(SD_SCK, HIGH);   // RTC latches the bit on the rising edge
    delayMicroseconds(1);
    if (thenRead && i == 7) pinMode(SD_MOSI, INPUT);  // release I/O before the RTC drives it
    digitalWrite(SD_SCK, LOW);
  }
}

// Receive one byte, lowest bit first. The RTC puts each bit out on the falling clock edge.
uint8_t rtcReadByte() {
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    delayMicroseconds(1);
    b |= digitalRead(SD_MOSI) << i;
    digitalWrite(SD_SCK, HIGH);
    delayMicroseconds(1);
    digitalWrite(SD_SCK, LOW);
  }
  return b;
}

// Start (on = true) or end (on = false) a transfer. Clock must be low when CE changes.
void rtcSelect(bool on) {
  digitalWrite(SD_SCK, LOW);
  digitalWrite(RTC_CE, on);
  delayMicroseconds(4);
}

// The DS1302 stores numbers as BCD: 0x59 means 59.
uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

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
  rtcWriteByte(0x00);                       // control: write-protect stays off
  rtcSelect(false);
}

// The time this sketch was compiled (from the __DATE__ and __TIME__ macros), used to
// set a new RTC. It is your PC's local time, so the RTC ends up in your local time too.
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
//   bottom: "MARK" while a button press waits to be logged
void drawOled(float tC, float hum) {
  if (!oledOk) return;
  time_t tt = nowEpoch();
  struct tm t;
  gmtime_r(&tt, &t);
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("%04d-%02d-%02d   %s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, markPending ? "MARK" : "");
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
// Order matters: OLED first (to show errors), RTC before the SD card (they share pins).
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nDHT SD logger (dummy tester)");

  pinMode(MARK_BTN, INPUT);  // plain INPUT: GPIO35 has no internal pull-up, the external 10k does the job
  dht.begin();

  // OLED first, so later errors can be shown on it. Logging still works without it.
  Wire.begin(OLED_SDA, OLED_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) { oled.setTextColor(SSD1306_WHITE); oled.clearDisplay(); oled.display(); }
  else Serial.println("WARNING: OLED not found, continuing without display.");

  // RTC: talk to it on the SD lines BEFORE SPI/SD take them over
  pinMode(SD_CS, OUTPUT);  digitalWrite(SD_CS, HIGH);   // keep the SD card deselected
  pinMode(RTC_CE, OUTPUT); digitalWrite(RTC_CE, LOW);
  pinMode(SD_SCK, OUTPUT); digitalWrite(SD_SCK, LOW);
  struct tm now;
  bool rtcOk = rtcRead(now);
  if (!rtcOk || FORCE_SET_RTC) {
    // New RTC or flat battery: start it at the compile time, then read it back to check
    Serial.println(rtcOk ? "RTC: forced set to compile time" : "RTC: not running, setting to compile time");
    rtcWrite(compileTime());
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

  // SD card on VSPI (takes over SD_SCK / SD_MOSI from here on)
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
  logFile.println("epoch_s,datetime,temp_c,humidity_pct,mark");  // CSV header
  logFile.flush();
  Serial.printf("Logging to %s\n", logPath);

  lastReadMs = millis() - READ_INTERVAL_MS;  // take the first reading straight away
}

// ================= loop(): check the button all the time, log every 2 s =================
void loop() {
  uint32_t ms = millis();

  // Button: LOW = pressed (the external pull-up holds it HIGH otherwise). 50 ms debounce.
  static bool lastBtn = HIGH;
  static uint32_t lastBtnMs;
  bool btn = digitalRead(MARK_BTN);
  if (btn != lastBtn && ms - lastBtnMs > 50) {
    lastBtn = btn;
    lastBtnMs = ms;
    if (btn == LOW) { markPending = true; Serial.println("Mark"); }
  }

  // Redraw the OLED twice a second so the clock ticks every second (the DHT is only read every 2 s)
  static uint32_t lastDrawMs;
  if (ms - lastDrawMs >= 500) { lastDrawMs = ms; drawOled(lastT, lastH); }

  if (ms - lastReadMs < READ_INTERVAL_MS) return;  // not time for a reading yet
  lastReadMs = ms;

  float tC  = dht.readTemperature();  // deg C, NAN if the sensor did not answer
  float hum = dht.readHumidity();     // % RH, NAN if the sensor did not answer
  if (isnan(hum)) tC = NAN;           // treat a half-failed read as a failed read

  // One CSV line: "epoch,YYYY-MM-DD HH:MM:SS,temp,humidity,mark". A failed read logs "nan"
  // so you can still see the logger is running while the sensor is unplugged.
  time_t tt = nowEpoch();
  struct tm t;
  gmtime_r(&tt, &t);
  logFile.printf("%ld,%04d-%02d-%02d %02d:%02d:%02d,%.1f,%.1f,%d\n", (long)tt,
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                 tC, hum, markPending ? 1 : 0);
  logFile.flush();                    // only one line every 2 s, so flush each time: a power cut loses nothing
  rowCount++;

  Serial.printf("%02d:%02d:%02d  T=%.1f C  H=%.1f %%  %s rows=%lu\n", t.tm_hour, t.tm_min, t.tm_sec,
                tC, hum, markPending ? "MARK" : "", (unsigned long)rowCount);
  if (isnan(tC)) Serial.println("WARNING: DHT read failed. Check the data wire, 3V3/GND and the DHT_TYPE setting.");

  markPending = false;
  lastT = tC; lastH = hum;
}
