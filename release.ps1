# Build a RigDeck release: one merged .bin per board for the browser flasher (docs/) and for esptool.
#
# Run it from the ESP-IDF PowerShell (start.bat) in the RigDeck folder:
#     .\release.ps1                 both boards: the 7B, then the NANO
#     .\release.ps1 -Board 7b       only the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (touch screen)
#     .\release.ps1 -Board nano     only the Waveshare ESP32-P4-NANO (no screen)
# Change version.txt first for a new version number. Beta builds end in "-beta" (e.g. 1.1.1-beta): they copy the
# console log to the microSD card unless the user turned that off. Stable builds use the plain number (1.1.1),
# where the console log is off unless the user turned it on.
#
# It writes, for the 7B (build folder build\, settings sdkconfig):
#   build\rigdeck-<version>.bin      complete image for address 0x0 (attach this to a GitHub release)
#   docs\firmware\rigdeck.bin        the same file, served by the flasher page
#   docs\manifest.json               tells the flasher page which file and version
#   docs\firmware\otadata.bin       blank update-slot selector (0x7C0000), a second part in the manifest
# and for the NANO (build folder build_nano\, settings sdkconfig.nano made from sdkconfig each time):
#   build_nano\rigdeck-nano-<version>.bin, docs\nano\firmware\rigdeck.bin, docs\nano\firmware\otadata.bin,
#   docs\nano\manifest.json
# The image ends after the program, so the log/settings partition is not touched when it is written.
param([ValidateSet("all", "7b", "nano")][string]$Board = "all")

$ErrorActionPreference = "Continue"   # idf.py and esptool write progress to stderr; failures are checked by exit code
Set-Location -Path $PSScriptRoot
$ver = (Get-Content version.txt -TotalCount 1).Trim()

function Publish([string]$buildDir, [string]$project, [string]$docsDir, [string]$label) {
    $out = "$project-$ver.bin"
    Push-Location $buildDir
    try {
        # flash_args holds the flash settings and every part with its address (bootloader, partition table, app).
        # The update-slot selector (otadata) sits far up the flash, past the settings partition: merging it
        # would pad the image over that partition and wipe it. It goes to the flasher as a separate part.
        $fa = Get-Content flash_args | Where-Object { $_ -notmatch "ota_data_initial" }
        [System.IO.File]::WriteAllLines((Join-Path (Get-Location) "flash_args_merge"), $fa)
        python -m esptool --chip esp32p4 merge-bin -o $out "@flash_args_merge"
        if ($LASTEXITCODE -ne 0) { throw "merge-bin failed ($label)" }
    } finally { Pop-Location }

    New-Item -ItemType Directory -Force -Path "$docsDir\firmware" | Out-Null
    Copy-Item -Force "$buildDir\$out" "$docsDir\firmware\rigdeck.bin"
    # Blank otadata: after a USB install the board starts the firmware just written (not an older web update).
    $otaPart = ""
    if (Test-Path "$buildDir\ota_data_initial.bin") {
        Copy-Item -Force "$buildDir\ota_data_initial.bin" "$docsDir\firmware\otadata.bin"
        $otaPart = ",`n        { `"path`": `"firmware/otadata.bin`", `"offset`": 8126464 }"
    }
    $manifest = @"
{
  "name": "RigDeck ($label)",
  "version": "$ver",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32-P4",
      "parts": [
        { "path": "firmware/rigdeck.bin", "offset": 0 }$otaPart
      ]
    }
  ]
}
"@
    [System.IO.File]::WriteAllText((Join-Path $PSScriptRoot "$docsDir\manifest.json"), $manifest)   # UTF-8, no BOM
    $size = (Get-Item "$buildDir\$out").Length
    Write-Host "Done ($label): $buildDir\$out ($([math]::Round($size/1MB,2)) MB), copied to $docsDir\firmware\rigdeck.bin" -ForegroundColor Green
}

if ($Board -ne "nano") {
    Write-Host "Building RigDeck $ver for the 7B ..." -ForegroundColor Cyan
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw "Build failed (7B)" }
    Publish "build" "rigdeck" "docs" "7B"
}

if ($Board -ne "7b") {
    Write-Host "Building RigDeck $ver for the NANO ..." -ForegroundColor Cyan
    # The NANO uses the same settings as this PC's 7B build (sdkconfig), with no touch screen and 16 MB of flash.
    $cfg = Get-Content sdkconfig | ForEach-Object {
        if ($_ -match '^CONFIG_IC7100_DISPLAY=') { '# CONFIG_IC7100_DISPLAY is not set' }
        elseif ($_ -match '^CONFIG_ESPTOOLPY_FLASHSIZE_\d+MB=y') { '# ' + ($_ -replace '=y$', '') + ' is not set' }
        elseif ($_ -match '^CONFIG_ESPTOOLPY_FLASHSIZE=') { 'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"' }
        else { $_ }
    }
    $cfg += 'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y'
    [System.IO.File]::WriteAllLines((Join-Path $PSScriptRoot "sdkconfig.nano"), $cfg)
    idf.py -B build_nano -D RIGDECK_BOARD=nano -D SDKCONFIG=sdkconfig.nano build
    if ($LASTEXITCODE -ne 0) {
        # The 7B release above is complete; a NANO problem does not hold it up.
        Write-Host "NANO build failed (the 7B release is not affected). See the messages above." -ForegroundColor Red
    } else {
        Publish "build_nano" "rigdeck-nano" "docs\nano" "NANO"
    }
}

Write-Host ""
Write-Host "To publish it on the flasher page:"
Write-Host "    git add docs version.txt"
Write-Host "    git commit -m `"Release $ver`""
Write-Host "    git push"
