# RTC Sync (Windows)

Sets the logger's DS1302 RTC to your PC's clock, accurate to a fraction of a second. Works with both `adxl375_sd_logger` and `dht_sd_logger`. Nothing to install: it uses the PowerShell that comes with Windows.

## Use

1. Plug in the board. **Close the Arduino Serial Monitor**, because only one program can use the COM port.
2. Double-click **`Sync RTC.bat`**.
3. Pick the board's COM port (the newest one is pre-selected; press Refresh if you plugged in after opening) and press **Sync RTC**.
4. Wait for `Done.` The board restarts with the new time and starts a new log file.

Opening the port may reboot the board, so the app waits up to 40 s for the logger to start (the ADXL375 logger can spend ~20 s trying WiFi/NTP first). It then picks a whole second, waits for it on the PC clock, and sends `YYYY-MM-DD HH:MM:SS`, the same command you could type in Serial Monitor.

To make sure the PC clock itself is right: Windows Settings > Time & language > Date & time > **Sync now**.

## If it doesn't work

- `Could not open COMx`: the Serial Monitor or another program has the port open.
- `The logger did not respond`: wrong COM port, the sketch isn't uploaded, or the board stopped on an error (shown in the log).
- Windows says the script can't run: right-click `RtcSync.ps1` > Properties > tick **Unblock** > OK, then try again.
