@echo off
rem Builds the Wi-Fi chip firmware and installs it over USB. The log goes to update_wifi_chip.log.
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%~dp0update_wifi_chip.ps1' 2>&1 | Tee-Object '%~dp0update_wifi_chip.log'"
pause
