# Build the firmware for the board's Wi-Fi chip (ESP32-C6). Double-click build_wifi_chip.bat, or run
#     .\build_wifi_chip.ps1
# from the ESP-IDF PowerShell (start.bat). The result is
#     docs\firmware\rigdeck-wifi-chip.bin
# which you send to the board from RigDeck's web page: Settings > Wi-Fi chip > Update.
$ErrorActionPreference = "Continue"   # idf.py writes progress to stderr; failures are checked by exit code
Set-Location -Path (Join-Path $PSScriptRoot "wifi_chip")
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
}
if (-not (Test-Path sdkconfig)) {
    idf.py set-target esp32c6
    if ($LASTEXITCODE -ne 0) { Write-Host "set-target failed" -ForegroundColor Red; exit 1 }
}
idf.py build
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed" -ForegroundColor Red; exit 1 }
New-Item -ItemType Directory -Force -Path ..\docs\firmware | Out-Null
Copy-Item -Force build\rigdeck_wifi_chip.bin ..\docs\firmware\rigdeck-wifi-chip.bin
$size = (Get-Item ..\docs\firmware\rigdeck-wifi-chip.bin).Length
Write-Host ""
Write-Host "Done: docs\firmware\rigdeck-wifi-chip.bin ($([math]::Round($size/1KB)) kB)" -ForegroundColor Green
Write-Host "Send it to the board: RigDeck web page > Settings > Wi-Fi chip > Update."
