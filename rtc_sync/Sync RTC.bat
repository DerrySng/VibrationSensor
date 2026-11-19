@echo off
rem Double-click to open the RTC Sync window. Uses the PowerShell built into Windows.
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File "%~dp0RtcSync.ps1"
