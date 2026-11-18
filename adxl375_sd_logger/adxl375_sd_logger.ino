// ADXL375 -> SD card CSV logger for ESP32-DevKitC (Arduino framework)
// + DS1302 RTC (shares the SD card's SCK/MOSI lines), SSD1306 OLED, zero button.
//
// Libraries (Library Manager): "Adafruit SSD1306" (pulls in Adafruit GFX + BusIO).
// Everything else ships with the ESP32 board package.
//
// Each boot creates a new file named from the RTC, e.g. /20261118_093000.csv
// (falls back to /log_000.csv ... if the RTC is missing).
// Columns: epoch_s (RTC time, seconds.microseconds), x_g, y_g, z_g

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <time.h>
#include "esp_timer.h"

// ---- Pins: PLACEHOLDERS, pin usage not decided yet. Change only this block. ----
// Avoid strapping pins (0, 2, 12, 15) and input-only pins (34-39) for outputs.
// ADXL375 (HSPI bus)
const int ADXL_SCK  = 14;
const int ADXL_MISO = 27;   // ADXL375 SDO
const int ADXL_MOSI = 13;   // ADXL375 SDA/SDI
const int ADXL_CS   = 26;
// SD card (VSPI bus)
const int SD_SCK    = 18;
const int SD_MISO   = 19;
const int SD_MOSI   = 23;
const int SD_CS     = 5;
// DS1302 RTC: SCLK -> SD_SCK, I/O -> SD_MOSI, CE (RST) on its own pin
const int RTC_CE    = 25;
// SSD1306 OLED (I2C)
const int OLED_SDA  = 21;
const int OLED_SCL  = 22;
const uint8_t OLED_ADDR = 0x3C;  // some modules use 0x3D
// Zero button: momentary switch between this pin and GND
const int ZERO_BTN  = 32;
// ---- end of pin placeholders ----

// ---- Settings ----
const uint32_t SAMPLE_INTERVAL_US = 625;   // 1600 Hz, matches the sensor's output data rate
const uint32_t FLUSH_INTERVAL_MS  = 1000;  // max data lost on power cut
const uint32_t PRINT_INTERVAL_MS  = 500;   // Serial preview rate
const float    G_PER_LSB          = 0.049; // ADXL375 datasheet: 49 mg/LSB
const int      ZERO_SAMPLES       = 160;   // 100 ms average when the zero button is pressed
const bool     FORCE_SET_RTC      = false; // true = set RTC to the compile time on boot (then set back to false and re-upload)

// ---- ADXL375 registers ----
const uint8_t REG_DEVID       = 0x00;  // reads 0xE5
const uint8_t REG_BW_RATE     = 0x2C;
const uint8_t REG_POWER_CTL   = 0x2D;
const uint8_t REG_DATA_FORMAT = 0x31;
const uint8_t REG_DATAX0      = 0x32;

SPIClass adxlSPI(HSPI);
const SPISettings adxlSettings(5000000, MSBFIRST, SPI_MODE3); // 5 MHz max, mode 3
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Preferences prefs;

File logFile;
char logPath[24];
uint32_t nextSampleUs, lastFlushMs, lastPrintMs, sampleCount;
int64_t bootEpoch, bootTimerUs;  // RTC time at boot + matching esp_timer value

float offX, offY, offZ;          // zero offsets in raw counts, saved in flash
int zeroN = -1;                  // >= 0 while averaging for a zero
int32_t sumX, sumY, sumZ;

// Latest values for the display task (core 0); written by loop() (core 1)
volatile float dispX, dispY, dispZ;
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
  adxlSPI.transfer(0x80 | (len > 1 ? 0x40 : 0x00) | (reg & 0x3F)); // read bit + multi-byte bit
  for (uint8_t i = 0; i < len; i++) buf[i] = adxlSPI.transfer(0x00);
  digitalWrite(ADXL_CS, HIGH);
  adxlSPI.endTransaction();
}

// ================= DS1302 (3-wire, LSB first) =================
// Only used in setup(), BEFORE the SD card takes over SCK/MOSI. With CE low the DS1302
// ignores the bus, and with SD_CS high the card ignores the RTC traffic.
void rtcWriteByte(uint8_t b, bool thenRead = false) {
  pinMode(SD_MOSI, OUTPUT);
  for (int i = 0; i < 8; i++) {
    digitalWrite(SD_MOSI, b & 1);
    b >>= 1;
    delayMicroseconds(1);
    digitalWrite(SD_SCK, HIGH);
    delayMicroseconds(1);
    if (thenRead && i == 7) pinMode(SD_MOSI, INPUT);  // release I/O before the RTC drives it
    digitalWrite(SD_SCK, LOW);
  }
}

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

void rtcSelect(bool on) {
  digitalWrite(SD_SCK, LOW);
  digitalWrite(RTC_CE, on);
  delayMicroseconds(4);
}

uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtcRead(struct tm &t) {
  uint8_t r[7];
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
  t.tm_mon  = bcd2dec(r[4] & 0x1F) - 1;
  t.tm_year = bcd2dec(r[6]) + 100;           // 2000-2099
  // All-0xFF / garbage (RTC not connected) fails these checks
  return t.tm_sec < 60 && t.tm_min < 60 && t.tm_hour < 24 &&
         t.tm_mday >= 1 && t.tm_mday <= 31 && t.tm_mon >= 0 && t.tm_mon < 12;
}

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

struct tm compileTime() {
  struct tm t = {};
  char mon[4];
  sscanf(__DATE__, "%3s %d %d", mon, &t.tm_mday, &t.tm_year);
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  t.tm_mon = (strstr("JanFebMarAprMayJunJulAugSepOctNovDec", mon) - "JanFebMarAprMayJunJulAugSepOctNovDec") / 3;
  t.tm_year -= 1900;
  mktime(&t);  // fills in tm_wday
  return t;
}

// ================= Display (runs on core 0 so I2C never stalls sampling) =================
void displayTask(void *) {
  for (;;) {
    int64_t nowS = bootEpoch + (esp_timer_get_time() - bootTimerUs) / 1000000;
    time_t tt = (time_t)nowS;
    struct tm t;
    gmtime_r(&tt, &t);

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.printf("%02d:%02d:%02d  %s\n", t.tm_hour, t.tm_min, t.tm_sec, zeroN >= 0 ? "ZEROING" : "OK");
    oled.println(logPath + 1);
    oled.setTextSize(2);
    oled.printf("X%7.2f\nY%7.2f\nZ%7.2f", dispX, dispY, dispZ);
    oled.display();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void halt(const char *msg) {
  Serial.println(msg);
  if (oledOk) {
    oled.clearDisplay(); oled.setTextSize(1); oled.setCursor(0, 0);
    oled.println(msg); oled.display();
  }
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nADXL375 SD logger");

  pinMode(ZERO_BTN, INPUT_PULLUP);

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
    Serial.println(rtcOk ? "RTC: forced set to compile time" : "RTC: not running, setting to compile time");
    rtcWrite(compileTime());
    rtcOk = rtcRead(now);
  }
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
  digitalWrite(ADXL_CS, HIGH);
  adxlSPI.begin(ADXL_SCK, ADXL_MISO, ADXL_MOSI, -1);

  uint8_t id;
  adxlRead(REG_DEVID, &id, 1);
  Serial.printf("ADXL375 DEVID: 0x%02X\n", id);
  if (id != 0xE5) halt("ERROR: ADXL375 not found. Check wiring (CS/SCK/SDO/SDA) and 3.3V.");

  adxlWrite(REG_DATA_FORMAT, 0x0B); // 4-wire SPI, right-justified, required bits set
  adxlWrite(REG_BW_RATE, 0x0E);     // 1600 Hz output data rate
  adxlWrite(REG_POWER_CTL, 0x08);   // measurement mode

  // Saved zero offsets
  prefs.begin("adxl", false);
  offX = prefs.getFloat("x", 0); offY = prefs.getFloat("y", 0); offZ = prefs.getFloat("z", 0);
  Serial.printf("Zero offsets (counts): %.1f %.1f %.1f\n", offX, offY, offZ);

  // SD card on VSPI (takes over SD_SCK / SD_MOSI from here on)
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
  if (!SD.begin(SD_CS, SPI)) halt("ERROR: SD card mount failed. Check card (FAT32) and wiring.");

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
  logFile.println("epoch_s,x_g,y_g,z_g");
  logFile.flush();
  Serial.printf("Logging to %s\n", logPath);

  if (oledOk) xTaskCreatePinnedToCore(displayTask, "oled", 4096, nullptr, 1, nullptr, 0);

  nextSampleUs = micros();
  lastFlushMs = lastPrintMs = millis();
}

void loop() {
  if ((int32_t)(micros() - nextSampleUs) < 0) return;  // not time yet
  int64_t tUs = esp_timer_get_time();
  uint32_t t = micros();
  nextSampleUs += SAMPLE_INTERVAL_US;
  // If an SD write stalled us, skip ahead instead of bursting stale samples. The gap shows in epoch_s.
  if ((int32_t)(t - nextSampleUs) > (int32_t)SAMPLE_INTERVAL_US) nextSampleUs = t + SAMPLE_INTERVAL_US;

  uint8_t b[6];
  adxlRead(REG_DATAX0, b, 6);
  int16_t rx = b[1] << 8 | b[0];
  int16_t ry = b[3] << 8 | b[2];
  int16_t rz = b[5] << 8 | b[4];

  // Zero button: on press, average the next ZERO_SAMPLES readings and use them as the new zero
  uint32_t now = millis();
  static bool lastBtn = HIGH;
  static uint32_t lastBtnMs;
  bool btn = digitalRead(ZERO_BTN);
  if (btn != lastBtn && now - lastBtnMs > 50) {   // 50 ms debounce
    lastBtn = btn;
    lastBtnMs = now;
    if (btn == LOW && zeroN < 0) { zeroN = 0; sumX = sumY = sumZ = 0; }
  }
  if (zeroN >= 0) {
    sumX += rx; sumY += ry; sumZ += rz;
    if (++zeroN == ZERO_SAMPLES) {
      offX = sumX / (float)ZERO_SAMPLES; offY = sumY / (float)ZERO_SAMPLES; offZ = sumZ / (float)ZERO_SAMPLES;
      prefs.putFloat("x", offX); prefs.putFloat("y", offY); prefs.putFloat("z", offZ);  // survives reboot
      Serial.printf("Zeroed. Offsets (counts): %.1f %.1f %.1f\n", offX, offY, offZ);
      zeroN = -1;
    }
  }

  float x = (rx - offX) * G_PER_LSB;
  float y = (ry - offY) * G_PER_LSB;
  float z = (rz - offZ) * G_PER_LSB;
  dispX = x; dispY = y; dispZ = z;

  int64_t e = bootEpoch * 1000000LL + (tUs - bootTimerUs);
  logFile.printf("%lld.%06lld,%.3f,%.3f,%.3f\n", e / 1000000, e % 1000000, x, y, z);
  sampleCount++;

  if (now - lastFlushMs >= FLUSH_INTERVAL_MS) {
    logFile.flush();
    lastFlushMs = now;
  }
  if (now - lastPrintMs >= PRINT_INTERVAL_MS) {
    Serial.printf("X=%7.2f g  Y=%7.2f g  Z=%7.2f g  samples=%lu\n", x, y, z, (unsigned long)sampleCount);
    lastPrintMs = now;
  }
}
