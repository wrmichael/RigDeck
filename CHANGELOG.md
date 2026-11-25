# Changes

## 1.0.1 (Beta)
- RigDeck name (Settings, Station): shown at the top right of the web page and in the browser tab, and used in
  download names: NAME-CALLSIGN-YYYYMMDD-HHMM-current.adi, -archive.adi, -console.txt.
- Do-not-work list (Settings, Choosing stations): listed stations are never called or answered, in any
  mode; W1ABC also covers W1ABC/P and VE3/W1ABC, XX9* a whole prefix. Their decodes are greyed out.
- Answer priority (off by default): when several stations call you or call CQ, pick by your order of
  POTA/WWFF, SOTA, DX (distance from the grid), special event (1x1 calls), weakest, strongest.
- Auto hunt answers CQ SOTA once per day, band and mode, like CQ POTA and CQ WWFF.
- The web page has an icon (browser tab, phone home screen).
- Settings export / import (web page, Settings, Device): a text file without passwords, named after the
  RigDeck; on import every change is listed and can be ticked off first (this unit's own items start unticked).

## 1.0.0 (2026-11-23) — first stable release
Highlights since the first public builds:
- FT8, FT4, FT-CW and PSK31; Auto CQ, Auto hunt, POTA mode ("Next QSO"), directed CQ, POTA/WWFF once per day.
- Radios: IC-7100, IC-705 (on the air), more Icom, Yaesu FTDX10 family, Kenwood/Elecraft (KX2/KX3/K3), QMX,
  No CAT sound cards (SignaLink) with RTS/DTR/VOX PTT (Digirig).
- Logging: per-station ADIF logs on the microSD card, Log4OM / UDP forwarding, editable logbook.
- Web page: everything the touch screen does, floating TX traffic light, TX watchdog (a visible control page is the
  operator present), dead man's switch (internet lost → finish the QSO and stop).
- Firmware updates over Wi-Fi (Stable / Beta, or a file), sent in CRC-checked pieces, with automatic rollback.
- Start-up screen with the station's callsign in gold, "RigDeck by AC9HP".
- Memory: LVGL and large allocations in PSRAM; internal RAM headroom ~190 kB.
- TX level remembered per radio; TX offset and even/odd remembered over a restart; DHCP host name from the hotspot
  name; callsign keyboard on the touch screen.

Known issues: see README → Status.
