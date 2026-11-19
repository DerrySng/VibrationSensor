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
| | SCK | GPIO18 | VSPI, **shared with DS1302 SCLK** |
| | MISO | GPIO19 | VSPI |
| | MOSI | GPIO23 | VSPI, **shared with DS1302 I/O** |
| | CS | GPIO4 | Hardwired on the base board |
| DS1302 RTC | VCC | 3V3 | Add a CR2032 coin cell (or the module's battery) so it keeps time |
| | GND | GND | |
| | CLK (SCLK) | GPIO18 | Shared with SD SCK |
| | DAT (I/O) | GPIO23 | Shared with SD MOSI |
| | RST (CE) | GPIO25 | Its own pin, must not be shared |
| SSD1306 OLED (I2C) | VCC / GND | 3V3 / GND | |
| | SDA | GPIO21 | |
| | SCL | GPIO22 | Address 0x3C (some modules use 0x3D). Jumper leads, not the OLED socket |
| Zero button | one leg | GPIO32 | Uses the internal pull-up |
| | other leg | GND | |

### How the RTC shares the SD pins

The DS1302 isn't really an SPI device. It uses a 3-wire bus with a single bidirectional data line, and it sends data LSB first. Because of that it can't sit on the SPI bus at the same time as the SD card. The sketch makes the sharing safe like this:

- The DS1302 gets its own CE pin. While CE is low, the DS1302 ignores the clock and doesn't drive its data line, so SD card traffic never reaches it.
- The RTC is read only once, at boot, before the SD card is started. During that read SD_CS is held high, so the card ignores the RTC traffic.
- Once the SD card is started it owns those pins. After that, the time comes from the ESP32's own timer, counted from the RTC time read at boot.
- Don't connect the DS1302 data line to MISO. Many SD modules leave their MISO buffer driving the line all the time, which would clash with the RTC.

If you'd rather keep the buses separate, connect the DS1302 to any 3 free GPIOs and change the pin constants. The code works either way.

## Setting the RTC

- A new RTC, or one whose battery was removed, is set automatically to the PC's time when the sketch was compiled. So upload right after compiling.
- To set it again later, change `FORCE_SET_RTC = true`, upload, then change it back to `false` and upload again. If you skip the second upload, every reboot resets the clock to the old compile time.

## OLED display

The display shows the sensor's live X/Y/Z status in g, updated 5 times a second:

```
09:30:12  OK
20261118_093000.csv
X   0.05
Y  -0.10
Z   0.00
```

The top line shows `ZEROING` while the zero button is working. The display runs on the ESP32's other core, so it doesn't interrupt the 1600 Hz sampling. If no OLED is found, the sketch still logs.

## Zero button

Press the button and hold the sensor still for 100 ms. The sketch averages 160 readings and uses that average as the new zero for X, Y and Z. That also removes the 1 g of gravity on whichever axis points up.

The offsets are saved in flash, so they're kept after a reboot. Pressing the button in the middle of a run shifts all the values that follow in that file.

## Output

Each boot creates a file named after the RTC time, for example `/20261118_093000.csv`. Without an RTC the files are named `/log_000.csv`, `/log_001.csv`, and so on.

```
epoch_s,x_g,y_g,z_g
1763458200.000000,0.049,-0.098,0.000
1763458200.000625,0.000,-0.049,0.049
```

`epoch_s` is the RTC time as Unix seconds with microsecond decimals, in whatever time zone the RTC was set to. In Excel you can convert it with `=A2/86400+DATE(1970,1,1)`.

## Expected Serial output (115200 baud)

```
ADXL375 SD logger
RTC: 2026-11-18 09:30:00
ADXL375 DEVID: 0xE5
Zero offsets (counts): 0.0 0.0 0.0
Logging to /20261118_093000.csv
X=   0.05 g  Y=  -0.10 g  Z=   1.03 g  samples=812
Zeroed. Offsets (counts): 1.2 -2.0 20.6
X=   0.00 g  Y=   0.05 g  Z=   0.00 g  samples=1612
```

Errors you might see:
- `ADXL375 DEVID: 0x00/0xFF` means the sensor isn't answering. Check its wiring and the 3.3 V supply.
- `RTC not found` means a CE, CLK or DAT wire is wrong. Logging continues, with timestamps counted from boot.
- `SD card mount failed` means the card isn't formatted FAT32, isn't inserted, or the wiring is wrong.

## Known limits

- When the card writes or flushes, the SD card can stall for a few ms. Samples taken during the stall are skipped, and the skip shows as a gap in `epoch_s`. The fix for that is the sensor's FIFO or a separate SD writer task.
- The timestamp's absolute accuracy is ±1 s, because the DS1302 only counts whole seconds. Within a file, timestamps are accurate to the microsecond relative to each other. The ESP32 crystal drifts by about 1 to 2 s a day, and the clock is re-synced to the RTC at each boot.
