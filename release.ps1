# Build a RigDeck release: one merged .bin for the browser flasher (docs/) and for esptool.
#
# Run it from the ESP-IDF PowerShell (start.bat) in the RigDeck folder:
#     .\release.ps1
# Change version.txt first for a new version number.
#
# It writes:
#   build\rigdeck-<version>.bin      complete image for address 0x0 (attach this to a GitHub release)
#   docs\firmware\rigdeck.bin        the same file, served by the flasher page
#   docs\manifest.json               tells the flasher page which file and version
# The image ends after the program, so the log/settings partition is not touched when it is written.

$ErrorActionPreference = "Continue"   # idf.py and esptool write progress to stderr; failures are checked by exit code
Set-Location -Path $PSScriptRoot
$ver = (Get-Content version.txt -TotalCount 1).Trim()
Write-Host "Building RigDeck $ver ..." -ForegroundColor Cyan

idf.py build
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$out = "rigdeck-$ver.bin"
Push-Location build
try {
    # flash_args holds the flash settings and every part with its address (bootloader, partition table, app).
    python -m esptool --chip esp32p4 merge-bin -o $out "@flash_args"
    if ($LASTEXITCODE -ne 0) { throw "merge-bin failed" }
} finally { Pop-Location }

New-Item -ItemType Directory -Force -Path docs\firmware | Out-Null
Copy-Item -Force "build\$out" docs\firmware\rigdeck.bin

$manifest = @"
{
  "name": "RigDeck",
  "version": "$ver",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32-P4",
      "parts": [
        { "path": "firmware/rigdeck.bin", "offset": 0 }
      ]
    }
  ]
}
"@
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot "docs\manifest.json"), $manifest)   # UTF-8, no BOM

$size = (Get-Item "build\$out").Length
Write-Host ""
Write-Host "Done: build\$out ($([math]::Round($size/1MB,2)) MB), copied to docs\firmware\rigdeck.bin" -ForegroundColor Green
Write-Host ""
Write-Host "To publish it on the flasher page:"
Write-Host "    git add docs version.txt"
Write-Host "    git commit -m `"Release $ver`""
Write-Host "    git push"
Write-Host "Optionally attach build\$out to a new release on GitHub (Releases > Draft a new release, tag v$ver)."
