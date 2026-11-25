# Changes

## 1.1.0 (Beta)
- **APRS** (web page, mode APRS): 1200 baud AFSK modem on the radio's FM audio, the radio set to FM with DATA
  on the APRS frequency (144.390 MHz by default). Stations heard with type, distance, direction, comment and
  path (plain, compressed and Mic-E positions); messages with acks (sent up to 3 times); packets heard and
  sent. Position beacons while TX is enabled, from a fixed position, a GPS (module or radio), or the grid.
  Settings, APRS: call-SSID, frequency, interval, symbol, comment, path, position, audio level.
- **CW trainer** (web page tab): character recognition with timing and adaptive practice (Koch or A-Z,
  characters added automatically), random groups, words (common, ham, own list), callsigns, QSO copy,
  sending practice (straight key or iambic paddles, decoded). Sound in the browser, or the radio's sidetone
  with break-in switched off over CAT and confirmed (Icom, Yaesu).
- **Listen**: the radio's receive audio in the browser (button next to the band buttons, with volume).
- RigDeck name in gold Audiowide at the top right of the web page.

## 1.0.3 (Beta)
- 60 m band button for FT8 (dial 5.357 MHz, USB-D), on the touch screen and the web page. No FT4 or FT-CW
  frequency on 60 m.

## 1.0.2 (Beta)
- Fixed: Save settings on the web page failed in 1.0.1 (the new priority list's tick boxes were sent as
  settings). Larger settings commands are accepted.

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
