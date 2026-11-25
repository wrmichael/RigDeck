# Changes

## 1.1.5-beta
- The RigDeck name (Settings) shows in gold on the touch screen, after the radio model at the top and in the APRS
  screen's title, so you can tell the units apart.
- APRS screen: **Get detailed map** / **Update map** button. RigDeck downloads the detailed map from the RigDeck
  site itself when it is on a home network with internet, and shows the progress under the map. The web page's
  button still works for a hotspot-only unit (the browser downloads it).
- Console log: why the board started (power on, software restart, crash ...) and each step of handing a new map
  to the touch screen.

## 1.1.4-beta
- **APRS on the touch screen:** APRS is now on the mode row. It opens an APRS screen with a map of the stations
  heard (country, state/province and Great Lakes outlines from Natural Earth, built in; range 25-1600 km), a
  station list, messages with an on-screen keyboard, station details, Enable TX / HALT TX / Beacon now.
- **Detailed APRS map:** Settings, APRS, "Get detailed map from GitHub": the browser downloads the map from the
  RigDeck site (Natural Earth 1:10m with US counties and highways, about 3.5 MB) and RigDeck stores it on the
  microSD card as RigDeck/map.rdm; "Remove detailed map" goes back to the built-in outlines. Counties and
  secondary roads show at 200 km range and closer, highways at 800 km and closer.
- APRS packets heard and sent are written to the microSD card, one file a day: RigDeck/aprs_YYYYMMDD.txt.
- APRS: pressing Beacon now again replaces the waiting position report instead of queueing another; a message's
  30-second retry wait starts when it has actually gone out.
- QMX / QMX+: RigDeck asks the radio for its firmware version (CAT VN;), shows it next to the model on the web
  page and writes it to the console log ("radio firmware: QMX 1_04_003").
- Settings, Device (touch screen and web page) lists the RigDeck version, the Wi-Fi chip firmware, the radio and
  its firmware.
- The last console lines before a restart now reach the microSD card.

## 1.1.3-beta
- Before a planned restart (update, Restart button) RigDeck closes the radio's serial port and switches its USB
  port off, so the radio sees the host leave cleanly.
- A USB device that does not start (QMX+ after a RigDeck restart) gets one USB port off/on; if it still fails, the
  screen and web page say "The radio on USB is not answering properly: switch the radio off and on".

## 1.1.2-beta
- Console log downloads are named RIGNAME-CALLSIGN-YYYYMMDD-HHMM-console.txt (the files on the card keep their
  names); before, the board's own file name won over the page's.
- A USB device that fails to start at power-up (seen with a QMX+) gets the USB port switched off and on, up to
  3 times, instead of staying dead until it is power cycled.

## 1.1.1-beta
- The console log now notes APRS packets heard and sent, beacons, messages and acks, mode changes, the CW
  trainer's radio sidetone (break-in asked off, confirmed, set back) and when a page starts listening.
- Copying the console log to the microSD card is now on by default in Beta versions (their version ends in
  "-beta") and off by default in Stable versions. A choice made in Settings, Device is kept either way.

## 1.1.0 (Beta)
- **CH340 / CH341 USB-serial** (VID 1A86): CAT for the FX-4CR (set it to TS-590S, 115200) and CH340 CAT or
  CI-V cables; 115200 is now among the CAT rates tried. Untested on hardware.
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
