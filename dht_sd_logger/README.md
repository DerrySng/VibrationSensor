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
| Mark button | one leg | GPIO35 | **Needs an external 10k pull-up**, see below |
| | other leg | GND | |
| 10k resistor | | GPIO35 to 3V3 | |

### Is GPIO35 OK for the button?

Yes, with one extra resistor. GPIO35 is input-only, which is fine for a button, and the base board doesn't connect anything else to it. But GPIO34 to 39 have no internal pull-up, so `INPUT_PULLUP` does nothing on them. Without a resistor the pin floats and reads random presses.

```
3V3 ──[10k]──┬── GPIO35
             │
          [button]
             │
GND ─────────┘
```

Not pressed: the 10k holds GPIO35 HIGH. Pressed: the button pulls it to GND (LOW). A 4.7k to 10k resistor works. If you'd rather not add a resistor, use a pin with an internal pull-up (GPIO32 or 33), change `MARK_BTN`, and change `pinMode(MARK_BTN, INPUT)` to `INPUT_PULLUP`.

If you use one of the base board's KEY1 to KEY4 buttons instead of a loose one, they also go to GND with no pull-up, so you still need the 10k.

## What the button does

The zero button doesn't make sense for a thermometer, so in this build it's a **mark** button. Press it and the next CSV line gets `mark = 1` (all others are 0), and the OLED shows `MARK` until that line is written. Use it to tag a moment, e.g. press it, then breathe on the sensor, and look for the humidity jump after the mark in the file. It also proves the button wiring works before you go back to the ADXL375 build.

## OLED display

```
2026-11-19   MARK
09:30:12
 24.0 C
 55.0 %
```

The date and clock update every second, temperature and humidity every 2 s. `DHT ERROR` means the sensor didn't answer.

## Output

One line every 2 s, flushed to the card each time, in a file named after the boot time (e.g. `/20261119_093000.csv`, or `/log_000.csv` without an RTC):

```
epoch_s,datetime,temp_c,humidity_pct,mark
1763544600,2026-11-19 09:30:00,24.0,55.0,0
1763544602,2026-11-19 09:30:02,24.0,56.0,1
```

A failed sensor read is logged as `nan`, so you can still see the logger is running with the sensor unplugged.

## Setting the RTC

Same as the ADXL375 logger: a new RTC is set to the compile time automatically. To set it again, change `FORCE_SET_RTC = true`, upload, then set it back to `false` and upload again.

## Expected Serial output (115200 baud)

```
DHT SD logger (dummy tester)
RTC: 2026-11-19 09:30:00
Logging to /20261119_093000.csv
09:30:00  T=24.0 C  H=55.0 %   rows=1
Mark
09:30:02  T=24.0 C  H=56.0 %  MARK rows=2
```

Errors you might see:
- `DHT read failed` means a data wire, power, or the `DHT_TYPE` setting is wrong.
- `RTC not found` and `SD card mount failed` mean the same as in the ADXL375 logger.
