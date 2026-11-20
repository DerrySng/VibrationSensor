# ADXL375 → SD card logger (ESP32-DevKitC)

The sketch is `adxl375_sd_logger/adxl375_sd_logger.ino`. Use the Arduino IDE with the ESP32 board package and select the **ESP32 Dev Module** board.
In Library Manager, install **Adafruit SSD1306**. It pulls in Adafruit GFX and BusIO automatically.

## Wiring (Prechin ESP32 base board)

Pins are checked against the base board schematic. The TF slot is soldered to GPIO4/18/19/23, so SD CS is 4. Wire the OLED with jumper leads, not the board's OLED socket, because that socket uses the SD card's GPIO18/23. Every pin is a constant in one block at the top of the sketch.

| Part | Part pin | ESP32 pin | Notes |
|---|---|---|---|
| ADXL375 | VS / VDDIO | 3V3 | 3.3 V only |
| | GND | GND | |
| | SCL/SCLK | GPIO14 | HSPI |
| | SDO | GPIO27 | HSPI MISO |
| | SDA/SDI | GPIO13 | HSPI MOSI |
| | CS | GPIO26 | |
| SD module | VCC | 3V3 (or 5V if the module has a regulator) | |
| | GND | GND | |
| | SCK | GPIO18 | VSPI |
| | MISO | GPIO19 | VSPI |
| | MOSI | GPIO23 | VSPI |
| | CS | GPIO4 | Hardwired on the base board |
| DS1302 RTC | VCC | 3V3 | Add a CR2032 coin cell (or the module's battery) so it keeps time |
| | GND | GND | |
| | CLK (SCLK) | GPIO16 | Own pin (not shared) |
| | DAT (I/O) | GPIO17 | Own pin (not shared) |
| | RST (CE) | GPIO25 | Its own pin, must not be shared |
| SSD1306 OLED (I2C) | VCC / GND | 3V3 / GND | |
| | SDA | GPIO21 | |
| | SCL | GPIO22 | Address 0x3C (some modules use 0x3D). Jumper leads, not the OLED socket |
| Zero button | one leg | GPIO32 | e.g. base board KEY1. Uses the internal pull-up, no resistor needed |
| | other leg | GND | KEY1 already goes to GND. For a button to 3V3 set `BTN_PRESSED = HIGH` (internal pull-down) |

### RTC pins

The DS1302 has its own three pins: CLK 16, DAT 17, RST/CE 25. GPIO16 and 17 also go to the base board's servo headers and LCD/OLED socket, so leave those empty.

## Setting the RTC

Wire the DS1302 to **CLK 16, DAT 17, RST 25**. The kit's own example uses different pins. Fit a CR2032 backup cell, or it loses the time at every power-off.

- **Automatic on upload:** the first boot after every new upload sets the RTC to the compile time, with nothing added, so it ends up behind by however long compiling and uploading took. Pressing RST or power-cycling does *not* change the clock. Only a new upload does. A brand-new RTC, or one whose battery was removed, is also set to the compile time.
- **One click from the PC:** double-click `rtc_sync/Sync RTC.bat`, pick the COM port and press Sync RTC. It does the step below for you, on the exact second.
- **Exact to the second:** open Serial Monitor at 115200 with the line ending set to **Newline**. Type a time a few seconds ahead of a reference clock (e.g. time.is) as `YYYY-MM-DD HH:MM:SS`, and press Enter exactly when the reference clock reaches that second. The sketch sets the RTC and restarts, and logging continues in a new file. A wrong format prints `Time not understood` and changes nothing.
- At each boot the sketch waits for the RTC's seconds to tick over before reading it, so the time starts on the exact second. Expect a drift of about 1 to 2 s a day after setting.

## OLED display

The display shows the clock, date and the sensor's live X/Y/Z in g, updated 5 times a second:

```
09:30:12  ZEROED
2026-11-19
X   0.05
Y  -0.10
Z   0.00
```

The top right shows `RAW` (no zero), `ZEROING` (averaging after a press) or `ZEROED` (zero on). The display runs on the ESP32's other core, so it doesn't interrupt the 1600 Hz sampling. If no OLED is found, the sketch still logs.

## Zero button (toggle)

- **First press:** hold the sensor still for 100 ms. The sketch averages 160 Z readings and uses that as the new zero for **Z only**, so Z goes from about 1 g (gravity) to 0 g. X and Y are never offset.
- **Next press:** turns the zero off and goes back to raw values. Each press after that toggles again.
- The zero isn't remembered. Every reset (RST or power-up) starts with raw values. While the zero is off, Z shows the raw reading, about 1 g with the sensor flat. Toggling in the middle of a run shifts all the values that follow in that file.

## Output

Each boot creates a file named after the RTC time, for example `/20261119_093000.csv`. Without an RTC the files are named `/log_000.csv`, `/log_001.csv`, and so on.

```
date,time,us,x_g,y_g,z_g
2026-11-19,09:30:00,000000,0.049,-0.098,0.000
2026-11-19,09:30:00,000625,0.000,-0.049,0.049
```

`date` and `time` are separate columns so Excel keeps the seconds. If it doesn't, set the time column's format to `hh:mm:ss`. `us` is the microsecond within that second, so samples 625 µs apart can be told apart.

## Expected Serial output (115200 baud)

```
ADXL375 SD logger
RTC: 2026-11-19 09:30:00
ADXL375 DEVID: 0xE5
Logging to /20261119_093000.csv
X=   0.05 g  Y=  -0.10 g  Z=   1.03 g  samples=812  lost=0  fifoFull=0
Zero ON. Z offset: 20.6 counts (1.01 g)
X=   0.00 g  Y=   0.05 g  Z=   0.00 g  samples=1612  lost=0  fifoFull=0
Zero OFF (raw values).
```

Errors you might see:
- `ADXL375 DEVID: 0x00/0xFF` means the sensor isn't answering. Check its wiring and the 3.3 V supply.
- `RTC not found` means a CE, CLK or DAT wire is wrong (CLK 16, DAT 17, RST 25). Logging continues, with timestamps counted from 1970-01-01.
- `SD card mount failed` means the card isn't formatted FAT32, isn't inserted, or the wiring is wrong.

## Known limits

- Sampling follows the sensor's own 1600 Hz clock. The ADXL375 keeps its latest 32 samples (20 ms) in its FIFO, and the sketch reads every one, so a CSV row is never a repeat and none is skipped. Each sample's time is counted back from the moment the FIFO was read, 625 µs apart, so `us` is accurate to within about one sample.
- SD writing runs on the other core. Samples wait in a RAM buffer of 4000 samples (2.5 s), so a slow card write doesn't stop sampling.
- Serial shows two counters. `lost` counts samples dropped because the RAM buffer filled up, meaning the card is too slow; try a faster card. `fifoFull` counts times the sensor FIFO filled up, so older samples may have been overwritten. Both should stay at 0.
- The timestamp's absolute accuracy is ±1 s, because the DS1302 only counts whole seconds. Within a file, timestamps are accurate to the microsecond relative to each other. The ESP32 crystal drifts by about 1 to 2 s a day, and the clock is re-synced to the RTC at each boot.
