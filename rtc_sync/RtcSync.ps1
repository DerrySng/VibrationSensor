# =====================================================================================
# RTC Sync - sets the logger's DS1302 RTC to this PC's clock, to the second.
# Works with both sketches: adxl375_sd_logger and dht_sd_logger.
#
# How it works:
#   1. Opens the board's COM port at 115200 baud (close the Arduino Serial Monitor first).
#   2. Waits until the logger is running (it may reboot when the port opens).
#   3. Waits for the start of a second on the PC clock and sends "YYYY-MM-DD HH:MM:SS",
#      the same command you could type in Serial Monitor.
#   4. The sketch writes the RTC, prints "RTC set to ..." and restarts with a new log file.
#
# Uses only what ships with Windows (PowerShell + .NET), so nothing to install.
# Start it by double-clicking "Sync RTC.bat".
# =====================================================================================

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$BAUD = 115200
$READY_TIMEOUT_S = 40   # the ADXL375 logger can spend ~20 s trying WiFi/NTP after a reboot

# ---------- window ----------
$form = New-Object System.Windows.Forms.Form
$form.Text = 'RTC Sync - VibrationSensor'
$form.ClientSize = New-Object System.Drawing.Size(460, 330)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false

$lblPort = New-Object System.Windows.Forms.Label
$lblPort.Text = 'COM port:'
$lblPort.Location = New-Object System.Drawing.Point(12, 16)
$lblPort.AutoSize = $true

$combo = New-Object System.Windows.Forms.ComboBox
$combo.Location = New-Object System.Drawing.Point(80, 12)
$combo.Width = 110
$combo.DropDownStyle = 'DropDownList'

$btnRefresh = New-Object System.Windows.Forms.Button
$btnRefresh.Text = 'Refresh'
$btnRefresh.Location = New-Object System.Drawing.Point(200, 10)
$btnRefresh.Width = 80

$btnSync = New-Object System.Windows.Forms.Button
$btnSync.Text = 'Sync RTC'
$btnSync.Location = New-Object System.Drawing.Point(300, 10)
$btnSync.Width = 145

$lblClock = New-Object System.Windows.Forms.Label
$lblClock.Location = New-Object System.Drawing.Point(12, 48)
$lblClock.AutoSize = $true
$lblClock.Font = New-Object System.Drawing.Font('Consolas', 12)

$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = 'Vertical'
$log.Location = New-Object System.Drawing.Point(12, 80)
$log.Size = New-Object System.Drawing.Size(436, 238)
$log.Font = New-Object System.Drawing.Font('Consolas', 9)

$form.Controls.AddRange(@($lblPort, $combo, $btnRefresh, $btnSync, $lblClock, $log))

# PC clock display, so you can compare it with the OLED
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 200
$timer.Add_Tick({ $lblClock.Text = 'PC time: ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') })
$timer.Start()

# ---------- helpers ----------
function Write-Log([string]$msg) {
  $log.AppendText($msg + "`r`n")
  [System.Windows.Forms.Application]::DoEvents()
}

function Load-Ports {
  $combo.Items.Clear()
  $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique
  foreach ($p in $ports) { [void]$combo.Items.Add($p) }
  if ($combo.Items.Count -gt 0) {
    $combo.SelectedIndex = $combo.Items.Count - 1   # the USB board is usually the newest port
  } else {
    Write-Log 'No COM ports found. Plug in the board and press Refresh.'
  }
}

# Read one line, or $null if nothing arrived within the port's ReadTimeout
function Read-BoardLine($port) {
  try { return $port.ReadLine().Trim() } catch { return $null }
}

# ---------- the sync ----------
function Sync-Rtc {
  if (-not $combo.SelectedItem) { Write-Log 'Pick a COM port first.'; return }
  $name = [string]$combo.SelectedItem

  $port = New-Object System.IO.Ports.SerialPort $name, $BAUD
  $port.NewLine = "`n"
  $port.ReadTimeout = 200
  $port.DtrEnable = $false   # don't pulse the auto-reset lines on purpose
  $port.RtsEnable = $false
  try {
    $port.Open()
  } catch {
    Write-Log "Could not open $name. Close the Arduino Serial Monitor (or anything else using the port) and try again."
    return
  }

  try {
    # 1. Wait until the sketch is in its main loop: both sketches print a line every 0.5-2 s,
    #    and print "Logging to ..." at the end of setup (the board may reboot when the port opens).
    Write-Log "Opened $name. Waiting for the logger to be running..."
    $deadline = (Get-Date).AddSeconds($READY_TIMEOUT_S)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
      $line = Read-BoardLine $port
      [System.Windows.Forms.Application]::DoEvents()
      if (-not $line) { continue }
      if ($line -match '^(X=|\d\d:\d\d:\d\d  T=)') { $ready = $true; break }   # ADXL375 / DHT data line
      Write-Log "  board: $line"
      if ($line -match '^Logging to') { $ready = $true; break }
      if ($line -match '^ERROR') { break }
    }
    if (-not $ready) {
      Write-Log 'The logger did not respond. Check the COM port, that the sketch is uploaded, and the errors above.'
      return
    }

    # 2. Pick a whole second 2 s from now (time to get ready), wait for it, then send.
    $now = Get-Date
    $target = (New-Object DateTime ($now.Ticks - ($now.Ticks % [TimeSpan]::TicksPerSecond))).AddSeconds(2)
    $text = $target.ToString('yyyy-MM-dd HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture)
    $port.DiscardInBuffer()
    while ((Get-Date) -lt $target) { }        # busy-wait the last bit for an accurate start
    $port.Write($text + "`n")
    Write-Log "Sent $text"

    # 3. Wait for the sketch's answer
    $deadline = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $deadline) {
      $line = Read-BoardLine $port
      if ($line -match '^RTC set to') { Write-Log "  board: $line"; Write-Log 'Done. The board restarts with the new time.'; return }
      if ($line -match 'not understood') { Write-Log "  board: $line"; return }
    }
    Write-Log 'No confirmation from the board. Is this sketch version new enough to accept the time over Serial?'
  } finally {
    $port.Close()
  }
}

$btnRefresh.Add_Click({ Load-Ports })
$btnSync.Add_Click({
  $btnSync.Enabled = $false
  try { Sync-Rtc } finally { $btnSync.Enabled = $true }
})

Write-Log 'Plug in the board, pick its COM port and press Sync RTC.'
Write-Log 'Tip: Windows Settings > Time & language > Date & time > "Sync now" makes sure the PC clock is right.'
Load-Ports
[void]$form.ShowDialog()
