# Update the board's Wi-Fi chip over the USB cable (no Wi-Fi needed). Double-click update_wifi_chip.bat.
#   1. builds the Wi-Fi chip firmware (build_wifi_chip.ps1)
#   2. writes it to the board's "c6fw" flash area at 0x600000 (the board's USB-C port, as for flashing RigDeck)
#   3. the board restarts, finds the new image and installs it on the Wi-Fi chip (about a minute),
#      then restarts again. RigDeck 0.9.15 or later must be on the board first (update_flash.bat).
# Logs, settings and RigDeck itself are not touched.
$ErrorActionPreference = "Continue"
& (Join-Path $PSScriptRoot "build_wifi_chip.ps1")
if ($LASTEXITCODE -ne 0) { exit 1 }
Set-Location -Path $PSScriptRoot
$img = [System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot "docs\firmware\rigdeck-wifi-chip.bin"))
if ($img.Length -gt 0x1C0000 - 16) { Write-Host "Image too large" -ForegroundColor Red; exit 1 }
# 16-byte header: "RDC6", image length (little-endian), 8 bytes zero; then the image.
$hdr = New-Object byte[] 16
[System.Text.Encoding]::ASCII.GetBytes("RDC6").CopyTo($hdr, 0)
[System.BitConverter]::GetBytes([uint32]$img.Length).CopyTo($hdr, 4)
$out = Join-Path $PSScriptRoot "wifi_chip\build\c6fw.bin"
[System.IO.File]::WriteAllBytes($out, [byte[]]($hdr + $img))
Write-Host "Writing the Wi-Fi chip image to the board (RigDeck stays as it is)..." -ForegroundColor Cyan
python -m esptool --chip esp32p4 write-flash 0x600000 $out
if ($LASTEXITCODE -ne 0) { Write-Host "Writing to the board failed (is the board's USB-C port connected and no monitor open?)" -ForegroundColor Red; exit 1 }
Write-Host ""
Write-Host "Done. The board restarts and installs the new Wi-Fi chip firmware (about a minute; the screen" -ForegroundColor Green
Write-Host "shows the progress), then restarts once more." -ForegroundColor Green
