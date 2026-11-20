# Vibration logger (fresh sketch)

`vibration_logger.ino`: ESP32 + ADXL375 at 1600 Hz to an SD card CSV, with a DS1302 RTC, an SSD1306 OLED and a zero button.
Board: **ESP32 Dev Module**. Library Manager: **Adafruit SSD1306** (pulls in Adafruit GFX and BusIO). No RTC library: the DS1302 is driven by hand-written code.

Pins are the same as `adxl375_sd_logger` (table in the main README): ADXL375 on HSPI 14/27/13/CS26, SD on VSPI 18/19/23/CS4, DS1302 CLK16/DAT17/CE25, OLED SDA21/SCL22, KEY1 on GPIO32 to GND.

## How logging starts and stops

Logging only runs while **ZEROED**. Every power-on or RST starts in **RAW**: the OLED shows live X/Y/Z, but nothing goes to the SD card.

1. Press KEY1. The OLED shows `ZEROING - hold still` while the sketch averages 1600 samples (1 s) of X, Y and Z.
2. The average is subtracted from all three axes, a new CSV file is created, and logging starts (`ZEROED - LOGGING`).
3. Press KEY1 again. The file is closed and it goes back to RAW (`RAW - not logging`).
4. The next press repeats steps 1 and 2 with a fresh average and a new file.

## CSV

Each file is named from the RTC at the moment logging starts, e.g. `/20261120_104206.csv`. The date only appears in the file name. A long run also starts a new file at the top of every hour (`/20261120_110000.csv`), so each file stays inside one date and well under FAT32's 4 GB limit. If the name already exists, `_1`, `_2` ... is added, so nothing is overwritten.

```
time,us,x_g,y_g,z_g,note
10:42:06,000000,0.049,-0.098,0.000,
10:42:06,000625,0.000,-0.049,0.049,
10:42:07,312500,,,,SD gap 3200 samples
10:42:09,000000,,,,dropped 120 samples
```

- `us`: microsecond within that second (samples are 625 us apart).
- Values in g, 3 decimals (the sensor counts in exact 0.049 g steps), with the zero subtracted.
- Error rows have empty x/y/z and say what was lost in `note`. Their time is where the gap starts.

About 40 bytes a row, so about 230 MB an hour.

## OLED (every 0.5 s, `OLED_MS`)

```
20/11/26  10:42:05
ZEROED - LOGGING
X   0.049
Y  -0.098
Z   0.000
```

Values are the latest sample read from the sensor, not from the SD card. When something is wrong, X/Y/Z are hidden and the screen says what to do:

| Screen | Meaning | What the logger does |
|---|---|---|
| `ADXL375 / NOT FOUND / check wiring` | sensor not answering | logs nothing, retries every 1 s, starts a new file when it is back (if ZEROED) |
| `NO SD CARD / insert FAT32 card` | no card (or not FAT32) | retries every 2 s; logging starts once a card works and you are ZEROED |
| `SD ERROR / logging stopped / check card` | card pulled or a write failed | retries every 2 s, continues the same file (new file if the hour changed) with an `SD gap` row |
| `SD FULL / swap card, / then press RST` | less than 1 MB free | stops logging until RST |

RTC problems, dropped samples and a missing OLED are only reported on Serial (115200).

## Setting the RTC

Each new upload sets the RTC to the compile time. For the exact second, use the RTC Sync app or type `YYYY-MM-DD HH:MM:SS` in Serial Monitor (Newline) and press Enter on that second. The logger closes its file, sets the RTC and restarts.

## Limits

- Data the card accepted but had not yet written when the card is pulled or power is cut is lost (at most about 1 s, the flush interval).
- Checking free space at mount can take a few seconds on some large cards. The 2.5 s RAM buffer covers most of it, and anything beyond shows up as a `dropped` row.
- Samples overwritten inside the sensor's own 32-sample FIFO can't be counted. They show up only as the `fifoFull` counter on Serial and should stay at 0.
