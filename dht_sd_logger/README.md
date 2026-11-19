# DHT11 → SD card logger (dummy tester, ESP32-DevKitC)

A copy of the ADXL375 logger with the accelerometer swapped for a DHT11 temperature/humidity sensor. Use it to check the SD card, DS1302 RTC, OLED and button wiring with a sensor that's hard to get wrong. The ADXL375 sketch is unchanged.

The sketch is `dht_sd_logger.ino` (in this folder). In the Arduino IDE pick the **ESP32 Dev Module** board. In Library Manager install:
- **Adafruit SSD1306** (pulls in Adafruit GFX and BusIO)
- **DHT sensor library** by Adafruit (pulls in Adafruit Unified Sensor)

Using a DHT22 instead? Change `#define DHT_TYPE DHT11` to `DHT22`.

## Wiring (Prechin ESP32 base board)

| Part | Part pin | ESP32 pin | Notes |
|---|---|---|---|
| DHT11 | VCC / GND | 3V3 / GND | Or plug it into the base board's DHT11 socket (J4) |
| | DATA | GPIO27 | Same pin as the kit's DHT11 example. J4 already has a 10k pull-up; jumper J4's signal to GPIO27. A bare DHT11 (not on a module or J4) needs a 10k from DATA to 3V3 |
| SD card | SCK / MISO / MOSI / CS | 18 / 19 / 23 / 4 | Base board TF slot, hardwired |
| DS1302 RTC | CLK / DAT / RST(CE) | 18 / 23 / 25 | Shares SD SCK/MOSI, read once at boot (same as the ADXL375 logger) |
| SSD1306 OLED | SDA / SCL | 21 / 22 | Jumper leads, not the board's OLED socket |
| Zero button (humidity) | one leg | GPIO32 | e.g. base board KEY1. No resistor needed (internal pull-up) |
| | other leg | GND | KEY1 already goes to GND on the base board |

### Button pin

The button is on GPIO32, which has built-in pull-up and pull-down resistors, so you don't need an external resistor. By default it's **active LOW** (`BTN_PRESSED = LOW`): the button connects GPIO32 to GND, the way the base board's KEY1 to KEY4 are wired, and the internal pull-up holds the pin HIGH when the button is released.

For a button to 3V3 instead, set `BTN_PRESSED = HIGH`. The sketch then switches to the internal pull-down automatically.

(GPIO35 was tried first. It has no internal pull resistors, so without an external one it floated and zeroed randomly.)

## What the button does

It's a **toggle zero for humidity only**. The first press makes the humidity reading at that moment the new 0 %. From then on, `humidity_pct` shows the change from that point, so it goes negative if the air gets drier. The next press turns the zero off and goes back to raw humidity, and so on. Temperature is never changed.

The OLED shows `ZEROED` in the top right while the zero is on. The zero isn't remembered. Every reset (the RST button or power-up) starts with raw humidity. Turning the zero on is ignored until the first valid reading arrives.

## OLED display

```
2026-11-19   ZEROED
09:30:12
 24.0 C
  0.0 %
```

The date and clock update every second, temperature and humidity every 2 s. `DHT ERROR` means the sensor didn't answer.

## Output

One line every 2 s, flushed to the card each time, in a file named after the boot time (e.g. `/20261119_093000.csv`, or `/log_000.csv` without an RTC):

```
date,time,temp_c,humidity_pct
2026-11-19,09:30:00,24.0,55.0
2026-11-19,09:30:02,24.0,0.0
```

A failed sensor read is logged as `nan`, so you can still see the logger is running with the sensor unplugged.

## Setting the RTC (accurate to the second)

**One click from the PC:** double-click `rtc_sync/Sync RTC.bat`, pick the COM port and press Sync RTC. It does these steps for you, on the exact second. Or by hand:

1. Open Serial Monitor at 115200 baud, with the line ending set to **Newline** (or Both NL & CR).
2. Type the time to set, a few seconds ahead of a reference clock (e.g. time.is), as `YYYY-MM-DD HH:MM:SS`, e.g. `2026-11-19 11:15:00`.
3. Press Enter exactly when the reference clock reaches that second.

The sketch writes the time to the RTC and restarts, and logging continues in a new file named with the correct time. A wrong format prints `Time not understood` and changes nothing.

At each boot the sketch waits for the RTC's seconds to tick over before reading it, so the log starts on the exact second. After setting, expect a drift of about 1 to 2 s a day from the DS1302 crystal. Re-set it every week or two if you need it to stay within a second.

**Automatic set on upload:** the first boot after every new upload sets the RTC to the compile time, as it is (no correction added). Pressing RST or power-cycling does *not* reset the clock. Only a new upload does. The clock ends up behind by however long compiling and uploading took, usually 30 to 60 s. For exact seconds, use the Serial Monitor method above.

A brand-new RTC, or one whose battery was removed, is also set to the compile time automatically.

## Expected Serial output (115200 baud)

```
DHT SD logger (dummy tester)
RTC: 2026-11-19 09:30:00
Logging to /20261119_093000.csv
09:30:00  T=24.0 C  H=55.0 %  rows=1
Humidity zeroed. Offset: 55.0 %
09:30:02  T=24.0 C  H=0.0 %  rows=2
```

Errors you might see:
- `DHT read failed` means a data wire, power, or the `DHT_TYPE` setting is wrong.
- `RTC not found` and `SD card mount failed` mean the same as in the ADXL375 logger.
