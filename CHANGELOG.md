# Changes

## 1.1.17-beta
- **FX-4CR and Kenwood TS-480 / TS-590 / TS-990: transmit sends RigDeck's audio.** PTT was "TX;", which these radios
  treat as microphone transmit, so they keyed up with no power out. Now "TX1;" (data send: USB audio). TS-2000 and
  Elecraft keep "TX;".
- **USB GPS receiver** (u-blox "G-Mouse" VK172 / VK-162 and other u-blox USB receivers, ESP32-P4 boards): plug it
  into the USB port (next to the radio: through a USB hub) and it sets the clock (Clock dot "gps-usb") and the APRS
  position once it has a fix. No setting needed. It goes ahead of the radio's GPS; a header module goes first.

## 1.1.16-beta
- Firmware 125 kB smaller again (1.1.15 pulled in a large number-formatting library for one log line).

## 1.1.15-beta
- **Packet / BBS over the air with a handheld (tested with QTerm on a BTECH UV-Pro):**
  - Packet TX delay (Settings, APRS; default 600 ms, was 300 ms): the first frame of each transmission was lost,
    so a lone UA never arrived and the station kept reconnecting.
  - The channel no longer counts as busy on noise with the squelch open (it stopped RigDeck from sending at all).
  - The link waits for an answer long enough for its own burst to go out (a 4-frame answer takes about 5 s at
    1200 baud), instead of polling every 4 s meanwhile.
  - The monitor shows when frames actually go on the air ("on the air: 3 packet frame(s), 1.9 s").
  - A station using the same call as RigDeck's own terminal gets the BBS's answers over the air.

## 1.1.14-beta
- Packet: a frame already waiting to go out is not queued a second time (while the channel was busy, repeated
  polls and answers piled up and then went out together).
- **BBS on the same RigDeck**: a program on RigDeck's own KISS port (QTerm, Winlink Express …) or RigDeck's own
  packet terminal can connect to that RigDeck's BBS. A radio cannot hear its own transmission, so those frames go
  straight to the BBS and back, not on the air (Enable TX not needed for that).
- **Clear the band activity list**: a Clear button on the web page (that browser's list), and on the touch screen
  hold the list for a second. New decodes keep coming.
- **Web page: transmit controls above the waterfall** (Enable TX, Halt, Tune, Manual / Auto CQ / Auto answer,
  TX Hz, slot), in one row, so they are in view without scrolling.
- **All stations page: compact station view** (Compact, on by default): smaller header, buttons and waterfall,
  shorter band list, no help lines, so a station fits in its tile. A station's page opened with ?compact=1 looks
  the same.
- Checks for new RigDeck releases once at start-up and then once a day (was every 6 hours).
- The "Update available" box no longer comes back after pressing Install (it reappeared while the update was
  downloading and being sent).

## 1.1.13-beta
- **Fewer internet requests, less memory pressure**: the check for new RigDeck releases ran about every 2 minutes
  instead of every 6 hours, and the K0DOG game check about every 8 minutes instead of daily (a long wait
  overflowed the timer arithmetic). Each check is an https request that briefly takes the board's scarcest memory;
  on IC705A that memory's low point crept from 113 kB to 77 kB over 4 hours.

## 1.1.12-beta
- **Waveshare ESP32-P4-NANO** (no screen) as a second board: the same RigDeck without the touch screen, for
  16 MB of flash, operated from the web page and the All stations page. The flasher page has a board choice; each
  board finds its releases in its own folder (the NANO in nano/) and installs only firmware for its own board
  (firmware name rigdeck / rigdeck-nano). release.ps1 builds both (-Board 7b or nano for one).

## 1.1.11-beta
- **Wi-Fi update no longer hangs part-way** (seen stuck at 22 %): a piece whose answer never comes is sent again
  after a time limit, the board gives up on a stalled piece after about 90 s instead of 10 minutes and asks for
  it again, and an update session survives such a pause.

## 1.1.10-beta
- **All stations page: status row.** One light per station at the top, checked every 10 seconds: green (radio
  connected, clock set), amber (answering, something to look at; hover for what), red (not answering), with the
  band and TX. Each station's title bar shows its firmware version. New /api/health on the board (open to other
  pages, read-only).
- The crash report is repeated once the clock is set, so it is also in the dated console log (lines from before
  the clock is known go to the undated console file).

## 1.1.9-beta
- **Crash report in the console log**: after a crash, the next start logs "last crash:" with what failed (the
  failed check with file and line, abort or exception address), so it shows in the console log downloaded from
  the web page, not only on the USB console.

## 1.1.8-beta
- **Smaller firmware**: the web pages are kept gzip-compressed in flash (index page 137 kB → 49 kB) and sent
  compressed; about 97 kB more room in the 3 MB slot used by USB installs.
- **APRS map only on the microSD card**: the outline map built into the firmware is gone (about 196 kB more
  room). The map (countries, states, lakes, US counties, highways; 3.5 MB) is fetched once onto the card: Get map
  on the APRS screen (RigDeck on a home network with internet), Get the map from the RigDeck site on the web
  page, Load a map file on the web page, or rigdeck-map.rdm copied into the RigDeck folder on the card. Until
  then the APRS screen shows rings and stations only.
- **Yaesu FTX-1 over USB**: the board could not reach the FTX-1's serial port and sound chip (full-speed chips
  behind the radio's high-speed USB hub). The radio USB port now runs at full speed (12 Mbit/s) with a patched copy
  of the USB host library (components/espressif__usb); setting usbFullSpeed=0 goes back to high speed.
  Unplugging or swapping radios while running reconnects without a restart.
- **Tune with the radio's tuner (Yaesu)**: Settings "Tune button (Yaesu)": Tune sends AC002 so the radio's
  own tuner or ATAS tunes, instead of a 10-second carrier.
- **Packet BBS** on the microSD card (Settings, APRS: Packet BBS on): a small mailbox on its own call-SSID
  (default the packet call with -1), up to 3 users at once over AX.25 connected mode, with the classic commands
  (L, LM, LB, R n, RM, S call, SB ALL, K n, I, J, B, ?). Personal mail and bulletins, kept one file per message
  in RigDeck/bbs. It answers only in APRS mode with Enable TX on. The Packet tab shows the BBS: who is connected,
  the messages, read, kill and post as the sysop.

## 1.1.7-beta
- **Packet radio** on the APRS modem (switch to APRS): a Packet tab with a connected-mode AX.25 2.0 terminal (BBS,
  nodes, keyboard to keyboard, digipeaters; window 4, retries, REJ, polling), unproto UI frames, a TNC-style
  monitor of every frame, and buttons to tune between the packet and APRS frequencies (beacons pause away from
  APRS). **KISS TNC over TCP** (port 8001) for Winlink Express, Pat, YAAC and other programs; their frames go out
  only with Enable TX on. Incoming connections are answered only with Enable TX on. Settings: packet call-SSID,
  packet frequency, incoming connections, KISS port. The demodulator now also passes the shortest control
  frames (17 bytes).
- **K0DOG**, the QSL-card dog game, in the CW trainer (button in Audiowide, opens /k0dog): copy the call, RST and
  band from a CW QSO between your operator and a station on the air, fill out the QSL card and the dog stamps it
  "PSE QSL" with its paw and sends it out the mail slot. Random dogs (mostly Shih Tzu and Maltese), random OM/YL
  operator, radio display with band, frequency and S-meter, four levels (Puppy to Contest), sniff hints,
  streaks, sounds (barks, stamp, mail slot) and optional voice. Stations sometimes send their own QSL back,
  signed by a cat or dog paw; they go in an album on the microSD card (/api/album). Calls from your log appear
  on the air. The game is not in the firmware: RigDeck downloads it from the site (docs/k0dog/) to the microSD
  card on the home network and serves it from there; no card, no K0DOG button. Also playable from the site
  (docs/k0dog.html).
  Start screen: the K0DOG club card (Dog Day Radio Amateur Radio Club); every 10th card sent, the club sends
  its own QSL card for the album.

## 1.1.6-beta
- **Update available** notice (light blue box) on the touch screen and the web page, with the running and the new
  version; Install on the web page, OK on the screen (not shown again for that release). The board looks at the
  RigDeck site every 6 hours on a home network; the page also looks itself. **Beta release notice** (Settings,
  Device) adds new Betas; on by default in Beta builds, off in Stable.
- **All stations in one window:** http://<any RigDeck>/multi shows up to 10 RigDecks side by side, each its own
  full web page in a frame (nothing lost). Add stations by address and name; columns, size, order, fill-the-window,
  open in a tab, an answering light. The list is kept in the browser and can be copied or bookmarked. Also as
  docs/multi.html to save and open from a computer. Link in Settings, Device.
  **Find stations:** RigDecks on the same home network find each other (a short UDP broadcast every 30 s,
  port 47731, with address, callsign, version and RigDeck name; /api/peers), so the page fills its list itself.
- New units name their hotspot RigDeck-XXXXXX (from the board's MAC address) instead of RigDeck, so two units
  never share a name. Units that already have settings keep their hotspot name.
- APRS position from the radio's own GPS: Icoms with GPS (IC-705, IC-9700, IC-905) are asked for their position
  over CI-V (command 23 00) every 30 seconds; beacons use it instead of the grid square's centre. The APRS
  screen and log say "position from the radio's GPS". Radios that do not answer are not asked again.

## 1.1.5-beta
- The RigDeck name (Settings) shows in gold on the touch screen, after the radio model at the top and in the APRS
  screen's title, so you can tell the units apart.
- APRS screen: **Get detailed map** / **Update map** button. RigDeck downloads the detailed map from the RigDeck
  site itself when it is on a home network with internet, and shows the progress under the map. The web page's
  button still works for a hotspot-only unit (the browser downloads it).
- Hotspot name: a note on the web page, touch screen and guide to give each unit its own hotspot name.
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
