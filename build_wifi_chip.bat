@echo off
rem Builds docs\firmware\rigdeck-wifi-chip.bin (firmware for the board's Wi-Fi chip). The log goes to build_wifi_chip.log.
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%~dp0build_wifi_chip.ps1' 2>&1 | Tee-Object '%~dp0build_wifi_chip.log'"
pause
