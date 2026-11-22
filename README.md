# RigDeck

**An FT8 station, logger and control head in one small touch-screen box.**

RigDeck is firmware for the Waveshare ESP32-P4 7-inch touch board. Plug it into your radio's USB
port with one cable and it:

- works **FT8 / FT4** contacts on its own (Auto CQ, Auto hunt) or by hand, with a live waterfall;
- runs **FT-CW**, FT8-style automatic contacts sent as high-speed Morse, and **PSK31**;
- logs **SSB / CW** contacts by hand, with CW macros sent through the radio's keyer;
- acts as a **control head**: band, frequency, mode, filters, levels and DSP from the touch screen;
- keeps an **ADIF log** on the board (current log + archive, downloadable), and can send each contact to
  a logging program over the network (WSJT-X UDP, ADIF over TCP/UDP);
- sets its clock from the radio's GPS, or from the browser;
- creates its own **Wi-Fi hotspot** with a web page that mirrors everything, so a phone or laptop
  can run it too.

Radios: Icom **IC-7100** (tested on the air), Icom **IC-705**, Yaesu **FTX-1** and QRP Labs **QMX / QMX+**
(tested against simulated radios so far; reports welcome).

> RigDeck started life as "IC7100-FT8". A controller upgraded from that firmware keeps its saved hotspot
> name until you change it in Settings; new installs use **RigDeck**.

## Status

Running on a Waveshare ESP32-P4-WIFI6-Touch-LCD-7B with ESP-IDF 6.1 and an IC-7100: the hotspot,
web page, CI-V control, USB audio in both directions and transmit have been confirmed on the air
by the first user. Other boards (ESP32-S3 in particular) are still untested, and so are the
IC-705, Yaesu FTX-1 and QRP Labs QMX drivers (tested against simulated radios only).

Checked on a PC with `sh test/build.sh`: FT8/FT4 encode/decode loopback, two simulated stations
completing contacts (also with a slow decoder and in FT4), PSK31 loopback, log management, and the
web page's script. `sh test/ui_sim.sh` renders the touch interface on a PC and taps its controls.

Things found on real hardware, already handled in this source:

- The IC-7100's audio codec has a 1,191-byte USB configuration descriptor; the USB host default
  limit is 256 (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=4096`).
- The radio's hub is a full-speed TI TUSB2046, so the ESP32-P4's high-speed port works with it
  as is. (The firmware asks for full-speed-only mode and carries on if ESP-IDF refuses.)
- The Waveshare board's ESP32-C6 ships with old esp-hosted firmware that streams several frames
  per SDIO read without announcing it. `components/espressif__esp_hosted` is a copy of
  esp_hosted 3.1.0 with a one-line patch for that (search for "IC7100-FT8 local patch").
  Updating the C6 firmware would make the patch unnecessary.

## Touch screen (Waveshare ESP32-P4-WIFI6-Touch-LCD-7B)

On this board the firmware also drives the 7 inch display, so no phone is needed to operate:

- Header: frequency, band/mode, signal and audio meters, link status, UTC clock.
- Waterfall: tap to set your transmit audio frequency (red marker).
- Band activity: tap a line to work that station. Blue = CQ, red = calling you, amber = your TX.
- Enable TX / HALT TX, Manual / Auto CQ / Auto hunt (answer CQs), CQ, Tune, Clear, TX slot.
- **Log: N** button: opens the logbook screen with the current log's contacts, the current and
  archive counts and **Clear current log** (asks first).
- Bottom row: FT8/FT4 and band buttons.

Callsign, grid, levels, network logging, PSK31 and the log downloads stay on the web page.
Touching the screen counts as "operator present" for the TX watchdog, the same as an open web page.
For ESP32-P4 boards without this display: `idf.py menuconfig` > IC-7100 FT8 controller > turn the
touch interface off.

## Hardware

The IC-7100's USB-B socket is a USB hub with two CP210x serial bridges (one is CI-V) and a USB
audio codec behind it. The board has to be a USB *host* that can drive a hub, a vendor serial chip
and isochronous audio together. An Arduino Uno cannot (no USB host, no Wi-Fi, 2 kB RAM), and a
classic ESP32 or a Heltec LoRa board cannot either (no USB host; Heltec V3 also has no PSRAM).

| Board | Verdict |
|---|---|
| **ESP32-P4 board with on-board ESP32-C6 Wi-Fi and a USB-A host socket** (Espressif ESP32-P4-Function-EV-Board, Waveshare ESP32-P4-WIFI6 and similar) | **Recommended.** 16 USB host channels, lots of PSRAM, plug the radio straight into the USB-A socket. |
| ESP32-S3 with PSRAM (e.g. ESP32-S3-DevKitC-1 N8R8 / N16R8) | Cheapest, experimental. The S3 has 8 USB host channels and this radio needs exactly 8 the way the firmware uses them (hub 2, three device control pipes 3, CI-V in/out 2, one audio stream 1). That is why receive and transmit audio are never open together. No margin. |

ESP32-S3 wiring (USB-A socket breakout, then a normal A-to-B printer cable to the radio):

| USB-A pin | ESP32-S3 |
|---|---|
| D- (white) | GPIO19 |
| D+ (green) | GPIO20 |
| VBUS (red) | 5V pin (the radio's hub looks for 5 V to know a host is there) |
| GND (black) | GND |

Power the board from its other USB port or a 5 V supply. A clip-on ferrite on the USB cable is
cheap insurance against RF getting into the board.

## IC-705

The IC-705 connects with one micro-USB cable as well. It appears as a single USB device with
two serial ports and audio (no hub), so it also fits within an ESP32-S3's USB limits. The
firmware finds the CI-V port and the CI-V address (A4h) by itself; nothing needs changing in
Settings when you swap radios. **Not yet tried on a real IC-705.**

- Radio: SET > Connectors > MOD Input > **DATA MOD: USB**; USB AF/IF Output > Output Select **AF**.
  Leave USB SEND/Keying OFF and the CI-V address at A4h.
- GPS clock: the IC-705 has its own GPS. SET > Connectors > USB (B) Function >
  **GPS Out: DATA -> USB (B)**, then on the web page turn on "Clock from radio's GPS".
- **Stop the 705 drawing power from RigDeck's USB port** (otherwise it starts charging, the board's
  5 V sags and the screen dims): SET > Function > **Charging (Power ON): OFF** and
  **USB Power Input (Phone, Tablet, PC): OFF**. Run the radio from its battery or a DC supply.
  Powering the board from a 5 V 2-3 A charger instead of a PC port also gives more headroom; a
  powered USB 2.0 hub between board and radio works too.

## Yaesu FTX-1

The FTX-1 connects with one USB-C cable. Inside it, as in the IC-7100, are a hub, a CP2105 dual
serial bridge and an audio codec; the firmware probes the serial ports, recognises Yaesu CAT
(`ID;` answered with four digits) and uses the first port (CAT-1). **Not yet tried on a real FTX-1.**

- Radio menu: the **CAT-1 rate** must match "FTX-1 CAT-1 rate" in Settings (38400 by default).
- FT8 etc. run in **DATA-U**, which the firmware selects. Set the radio's data-mode audio input to
  USB and the USB audio levels as in any WSJT-X guide for the FTX-1.
- RF power: the field head runs 1-10 W, the optima (SPA-1) 5-100 W; the percentage in Settings
  is converted to watts for the head the radio reports.
- Logger CW macros are sent with the radio's keyer (text written to keyer memory 1, then played,
  50 characters at most). Break-in is turned on if it is off, otherwise only sidetone is heard.
- The radio screen covers frequency, modes (including C4FM as "DV"), DATA, AF/RF/squelch/mic/
  power, preamp (IPO/AMP1/AMP2 on HF), attenuator, noise blanker, DNR, auto notch, AGC,
  speech processor and VOX. Filter buttons have no FTX-1 equivalent and are ignored.

## QRP Labs QMX / QMX+

The QMX appears as one USB device (STMicroelectronics VID 0483) with a CDC serial port and 24-bit
48 kHz USB audio; the firmware speaks its Kenwood TS-480 style CAT and converts audio to and from
24 bits. **Not yet tried on a real QMX.**

- FT8 etc. use the QMX's **DIGI** mode (`MD6`), which the firmware selects.
  Transmit is keyed with `TQ1`, and the QMX itself generates the RF from the audio tone.
- The QMX has no CAT commands for power, gain or filters, so on the radio screen those buttons and
  sliders have no effect. Frequency, band, mode
  (LSB/USB/CW/DIGI), A/B and keyer speed work.
- Logger CW macros use the QMX keyer (`KY`), sent in 24-character pieces.

## SSB / CW logger and CW macros

A screen (button **SSB/CW** on the operating screen) and a web tab (**SSB / CW log**) for contacts
made by hand. Opening the screen turns automatic FT8 transmit off.

- The **frequency** box (MHz) follows the radio. Type a frequency into it (14.250, or 14250 kHz) and
  it stays as typed, shown in white instead of amber, until the contact is logged; then it follows
  the radio again. With no radio connected, type the frequency.
- **SSB** / **CW** choose the mode that is logged, whatever mode the radio is in, until the contact
  is logged; then the mode follows the radio again. When a radio is connected they also switch it
  (LSB below 10 MHz except 60 m, USB above), since the CW macros need the radio in CW.
- Type the callsign, reports, name and a note, press **LOG**. SSB contacts get an LSB/USB submode. Empty reports are logged as
  59 (voice) or 599 (CW). A callsign already worked on this band is flagged in red.
- Six **macro** buttons send CW through the radio's own keyer (radio must be in CW mode); **Stop CW**
  aborts. The keyer speed is shown and set with − / +. On the web page you can also type free text.
- Macros are edited in Settings as `Label|text`. In the text, `{CALL}` is the callsign box,
  `{RST}` the Sent box (9 is sent as N: 599 → 5NN), `{MYCALL}`, `{GRID}` (4 characters) and
  `{REF}` (the park reference, MY_SIG_INFO) come from Settings. Defaults: CQ, His call, Report,
  TU 73, My call, AGN?.
- "Keyer speed for the logger" in Settings is sent to the radio whenever it connects (0 leaves the
  radio's own speed alone).
- The NAME field is written to ADIF as NAME; the note as COMMENT.

## Settings on the touch screen

The gear button at the top right of the operating screen opens **Settings**: everything on the web
page's Settings tab, in eight tabs (Station, Operating, Radio, CW, Logging, Wi-Fi, Clock, Device). Tap a box to type
(a keyboard rises from the bottom; ✓ or ✕ puts it away), choose from lists, then press **Save**.
Invalid values are refused with the field named on the status line. Password boxes stay empty: leave
them empty to keep the current password. **Restart** restarts the controller (needed after Wi-Fi,
baud-rate and GPS changes). CW macros have a label box (the button name) and a text box.

**Device** tab (also on the web page, Settings, Device): **Brightness** (changes at once),
**Contrast** (100 % = as designed; higher makes panels darker and text brighter, lower greys
everything; applies after a restart, which it offers), where the log is and the free space in the board's memory and on the microSD card
(shown separately), a rough count of contacts that still fit, and **Erase all logs** (asks twice;
on the web page you type NUKE). **Format microSD card** (asks twice; on the web page you type
FORMAT) erases the card and formats all of it as FAT32: this station's log is parked in the board's
memory meanwhile and put back; other files on the card are lost. Use it for a new card (most
64 GB and larger cards come as exFAT, which RigDeck cannot read) or one partitioned small. The
Device page shows the card's size and its file system's size, so a small partition shows up there.
**Console log to the card** (on by default): everything the board prints on its USB console also
goes to `RigDeck/logs/console_YYYYMMDD_HHMM.txt` on the card, as it happens (a new file at each
start, when the clock gets set and each UTC day; a time mark every minute). Without a clock the
file is `console_19761018_0000.txt`. Files dated more than 7 days back are removed.

## Choosing the radio

Settings > Radio is **Find automatically** by default: every serial port on the USB bus is tried as
Icom CI-V (configured address, 88h, A4h), as Yaesu CAT and as QMX CAT, and the first that answers
is used. You can swap radios without changing anything. Pick a specific radio there only if the
search ever settles on the wrong one. The radio-GPS clock option works with the Icoms only (any radio
works with a GPS module on the board, see below).

## Radio settings (SET > Connectors)

- **DATA MOD: USB** (default is ACC; without this the radio transmits silence)
- **USB MOD Level:** start around 20 %
- **USB AF Level:** around 50 %; if your firmware has **ACC/USB Output Select**, set it to AF
- **CI-V address 88h**, CI-V baud 19200 or Auto (both are the firmware's defaults; changeable in Settings)
- USB SEND / USB keying: OFF (PTT is done with a CI-V command, the serial control lines are held idle)

The firmware puts the radio in USB-D (USB with DATA mode on) whenever you pick a band.

## Install without building (browser flasher)

Open **https://wrmichael.github.io/RigDeck/** in Chrome or Edge, connect the board's USB-C port to the
computer, and press **Install RigDeck**. When updating, leave "Erase device" unticked to keep the log
and settings. The same page offers the .bin for esptool (address 0x0).

### Making a release (maintainer)

1. Put the new version number in `version.txt`.
2. In the ESP-IDF PowerShell, in the RigDeck folder: `.\release.ps1`. It builds, merges bootloader,
   partition table and program into `build\rigdeck-<version>.bin`, and copies it to `docs\firmware\`
   with an updated `docs\manifest.json`.
3. `git add docs version.txt`, `git commit -m "Release x.y.z"`, `git push`. GitHub Pages (Settings >
   Pages > Deploy from branch `main`, folder `/docs`) then serves the new firmware within a minute or two.
4. Optionally attach the .bin to a GitHub release.

The version shows on the touch screen (Settings) and on the web page (Settings tab, bottom).

## Build and flash

Built and tested with **ESP-IDF 6.1** (5.5.3 or newer should work; the USB hub support comes from
Espressif's `usb` component 1.6+, fetched automatically on first build). A ready-to-flash .bin and a
browser flasher are planned, so you will not need to build it yourself.

```
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

On Windows with Espressif's EIM installer, `start.bat` opens a PowerShell with ESP-IDF 6.1 ready
(edit the path inside if your install differs).

- ESP32-P4: Wi-Fi runs on the board's ESP32-C6 via `esp_hosted`. Its default pins match
  Espressif's Function-EV board; for other boards check `idf.py menuconfig` > ESP-Hosted. The C6
  must carry esp-hosted slave firmware (boards normally ship with it).
- ESP32-S3 with quad PSRAM (N4R2 etc.): delete the `CONFIG_SPIRAM_MODE_OCT` line in
  `sdkconfig.defaults.esp32s3`.
- Flash size is set to 8 MB in `sdkconfig.defaults`.

## Using it

1. Join the hotspot **RigDeck**, password **ft8ft8ft8**, and open **http://192.168.4.1**.
2. Settings tab: enter callsign and grid, change the hotspot password, Save. It refuses to
   transmit until callsign and grid are set.
3. Header dots: USB (serial port found), CI-V (radio answering), Audio (codec open), Clock.
4. **Clock.** FT8 needs UTC within a few tenths of a second and the board has no clock. On the
   hotspot it copies your phone's or laptop's clock whenever the page is open, so the device is
   only as accurate as that. If you also give it your home Wi-Fi (Settings > "Also join"), it uses
   NTP instead. If every decode shows the same DT offset, nudge with the trim buttons.
5. **Levels.** Receive: the "Audio in" meter should sit roughly -30 to -10 dBFS on band noise
   (adjust USB AF Level). Transmit: press **Tune** (10 s carrier, into a dummy load) and raise
   "TX audio level" / USB MOD Level until power stops rising, then back off so ALC stays near zero.
6. Pick a mode and band. Decodes appear at the end of each 15 s slot.
   - **Manual:** tap a decode to call that station (this enables TX); the sequence then advances
     by itself and TX switches off after 73. Tx1-Tx6 buttons override the next message.
   - **Auto CQ:** press Enable TX; it calls CQ, works whoever answers (strongest first), logs,
     and calls CQ again.
   - **Auto answer CQs:** it calls the strongest plain CQ it has not worked on this band, works
     it, logs, and looks for the next. Directed CQs (CQ DX, CQ POTA...) are left to you.
   - **Halt TX** unkeys immediately in every mode.
7. PSK31: click the waterfall on a signal, read the text, type and Send. Log with the form.

Safeguards: transmitter forced off if a transmission overruns its slot or the USB link drops;
automatic transmission stops after 6 minutes (configurable) with no browser connected; a QSO
attempt is dropped after 5 unanswered repeats. You are still the control operator: stay with it,
and check what your licence allows for automatic operation.

## FT-CW: FT8-style contacts in Morse code

An experimental mode of this project's own (nothing else on the air speaks it, so it needs two
of these controllers). Select **CW** on the touch screen or **FT-CW** on the web page.

- Works exactly like FT8 from the operator's side: 15 s slots, even/odd, the same messages
  (CQ, grid, dB report, R+report, RR73, 73), Manual / Auto CQ / Auto hunt, tap a decode to call.
- On the air it is machine-timed Morse on an audio tone, radio in USB-D like FT8: for example
  `CQ AC9HP EM69 7Q`. The last two characters are a check group; a copy that does not match it
  (or does not read as a standard message) is discarded, and the decoder uses it to repair one or
  two marginal dots. Reports are in dB like FT8. Contacts are logged with MODE CW.
- The decoder is a matched filter for the dot length, run on every audio frequency at once, so
  several stations 100 Hz or more apart are decoded in the same slot.
- **Speed** (Settings > "FT-CW speed", default 40 wpm) must be the same at both stations.
  20 to 50 wpm; the longest standard messages need 35 wpm or more to fit a slot, and the
  firmware refuses to send a message that will not fit.
- Sensitivity in simulation: about -5 dB (2500 Hz reference) at 40 wpm, -7 dB at 30 wpm.
  That is roughly 15 dB less sensitive than FT8: it needs signals you could copy by ear.
- The band buttons use dial frequencies picked to stay clear of the FT8/FT4 channels
  (1.835, 3.565, 7.065, 10.125, 14.065, 18.095, 21.065, 24.910, 28.065, 50.095, 144.065 MHz,
  signal about 1.5 kHz higher). These are only a starting point: tune anywhere you both agree
  on with the Radio screen, and check they suit your licence and band plan.

- The decoder follows the radio path: its decision level tracks fading (QSB), and it reads each
  element by its length as well as on a fixed dot grid, so a sender whose audio clock is slightly
  off, or a short glitch in the USB audio, no longer loses the whole message (0.9.26).

## Radio screen (control head)

**Radio** on the operating screen opens a front panel for the transceiver:

- Frequency: step up/down (10 Hz to 1 MHz steps, hold to repeat), band buttons (160 m to 70 cm),
  or tap **Enter** / the frequency for a keypad (MHz with a point, kHz without).
- Mode: LSB, USB, CW, AM, FM, RTTY, DV, the DATA switch and filter 1/2/3; VFO A/B exchange.
- ATT, preamp (off/1/2), noise blanker, noise reduction, auto notch, AGC (fast/mid/slow),
  speech compressor, VOX.
- Sliders: AF gain, RF gain, squelch, RF power, mic gain, NR level.
- Signal meter, and power / ALC while transmitting.

The values are read back from the radio, so the screen follows the radio's own knobs. A control
shows "--" or stays grey if the radio does not report it. Changing mode here switches automatic
transmitting off; tapping a band on the operating screen puts the radio back in USB-D. The same
commands serve the IC-7100 and IC-705 (the attenuator is 12 dB on one and 20 dB on the other).

## Automatic TX audio level (ALC)

Driving the radio's ALC into the red compresses the audio, which widens an FT8 signal and spreads
splatter over other stations. With Settings > Operating > "Lower audio when ALC shows" **On** (the
default), RigDeck reads the radio's ALC meter about every 0.4 s while transmitting. At the end of
each FT8/FT4 transmission it looks at the upper quarter of those readings: above the ALC zone the
audio goes down a step (a reading deep in the red twice in a row lowers it at once); well inside the
zone it rises again, never above the "TX audio level (maximum)" setting. Single jumpy readings do not
move it. If lowering the audio does not lower the ALC (the radio's own settings hold it up: RF power,
compressor, USB MOD level), RigDeck stops lowering and says so on the status line rather than turning
the audio down to nothing. FT-CW and PSK31 use the level learned with FT8 on that band without
changing it (Morse keying makes the ALC jump on every element). The level is kept for each radio and
band (file `alc.txt` with the log), so it survives restarts; changing the maximum or turning the
feature off starts it fresh.

- Set the radio's power as you want it (e.g. 5 W on the IC-705); the audio level only removes ALC.
- The level in use shows as "audio NN %" next to Po/ALC while transmitting.
- Works with the Icoms and the FTX-1. The QMX has no ALC; it always gets full audio (see QMX).

## Keeping the time

FT8 needs the clock within about a second. RigDeck sets it from, best first: a GPS module with PPS,
the radio's GPS, NTP (when it has joined a Wi-Fi network with internet), or the browser of the
phone/PC showing the web page. The Clock dot on the web page shows which one it is using.

- **Restarts keep the time.** Every time the clock is set it is also written into the chip's own
  clock, and read back at the next boot (source "rtc"). A restart, a firmware update over USB or a
  crash recovery therefore starts with the right time. It is replaced as soon as a better source
  shows up.
- **Power off loses it** unless the chip's backup supply keeps running. The board has a holder for a
  rechargeable ML1220 cell (Waveshare: rechargeable 3 V cells only) wired to the chip's VBAT pin, but
  ESP-IDF's support for running the clock from it is still experimental, so treat that as untested.
  A GPS module is the dependable answer for portable use.

## GPS module on the board (best timing, no internet)

Any 3.3 V-logic NMEA GPS module works; one with a **PPS** output (u-blox NEO-6M / NEO-M8N, ATGM336H
boards and similar, about $10-15) gives timing to within a few milliseconds.

- Wiring to the board's GPIO header: module **TX → GPIO3**, **PPS → GPIO4**, **GND → GND**,
  **VCC → 3.3 V or 5 V** as the module requires. The module's RX is not needed. Other free GPIOs
  can be chosen in Settings; 0/1, 7/8, 14-19, 37/38 and 54 are taken by the board.
- Settings > Clock (touch screen) or the web Settings tab: **GPS module on the header: On**, baud
  (most modules ship at 9600), the two GPIO numbers (PPS -1 if not wired). Save and restart.
- The monitor log shows once a minute whether the module sends time and how many PPS pulses it
  saw. The module needs a sky view for its first fix; after that the Clock dot shows "gps-pps".
- Without PPS the sentence arrival time is used, adjusted by "GPS sentence delay" (see below).

## Wi-Fi tips

- **Joining your home network** (for internet time and a logging PC on your LAN): Settings > Wi-Fi,
  enter the home network name and password, press **Save + connect**. No restart is needed. The line
  above the buttons shows the state: connecting, connected with its address and signal, or retrying
  with the reason (network not found, wrong password, signal too weak...). **Disconnect** leaves the
  network until you connect again or restart. The board's Wi-Fi is **2.4 GHz only**: a 5 GHz-only
  network is "not found". While joined, the hotspot moves to the home network's channel; phones
  connected to the hotspot may reconnect once.

- Phones often leave a hotspot that has no internet. On Android, when it asks, choose **Keep
  Wi-Fi connection / Stay connected**, and turn off "Switch to mobile data" (or "Adaptive Wi-Fi")
  for the RigDeck network; on iPhone, turn off **Wi-Fi Assist**.
- Keep the board within a few metres of the phone or laptop; the antenna is on the board's edge.
- If the hotspot itself disappears (the network is gone from the list), send the monitor log: the
  Wi-Fi chip on these boards runs older firmware, and the log shows whether that is the cause.
- While joined, RigDeck checks every 20 s that the router still answers; after a minute of silence
  it leaves and joins again.

## Updating the Wi-Fi chip

The board has two chips: the ESP32-P4 runs RigDeck, and an ESP32-C6 does the Wi-Fi. Boards ship
with old Wi-Fi chip firmware (it reports version 0.0.0 or none), which is the likely cause of
dropped connections. RigDeck can update it over the link between the two chips; no cables or
buttons.

**Over USB (recommended, no Wi-Fi needed):** connect the board's USB-C port to the PC as for
flashing RigDeck (RigDeck 0.9.15 or later must already be on the board) and double-click
`update_wifi_chip.bat`. It builds the Wi-Fi chip firmware, writes it to a spare flash area on the
board, and the board installs it on the Wi-Fi chip at its next start (the status line shows the
progress), then restarts. Logs and settings are kept. It is installed once; a failing image is
retried at most 3 times.

**Over Wi-Fi, from the web page:**

1. Get `rigdeck-wifi-chip.bin`: from the release page, or build it by double-clicking
   `build_wifi_chip.bat` (ESP-IDF installed as for RigDeck; the result is
   `docs\firmware\rigdeck-wifi-chip.bin`). It is built from the same ESP-Hosted version RigDeck
   uses (`wifi_chip/`), so the two always match.
2. Join the **RigDeck hotspot** and open http://192.168.4.1 (more reliable than the home network
   for this). Settings tab > **Wi-Fi chip** shows the current firmware.
3. Choose the file and press **Update Wi-Fi chip**. Transmitting stops, the file is sent (about a
   minute), the chip checks it, and RigDeck restarts. Reconnect and check the version.

The new firmware is written to the chip's spare slot; the chip only switches to it once it has
arrived complete and checked, so an interrupted upload leaves the old firmware running. If the
page says the chip **refused** the update, its old firmware cannot be updated this way and nothing
was changed. Keep the board powered during the update.

## Clock from a GPS on the radio

The IC-7100 has no GPS of its own, but if you have a GPS receiver on its DATA1 jack (as used
for D-STAR), the radio can pass the GPS sentences to its second USB serial port and the firmware
sets its clock from them:

- Radio: SET > Connectors > USB2/DATA1 Function > **GPS Out: DATA1 -> USB2**, and note the
  **DVdat/GPS Out Baud** (4800 or 9600). In the GPS set mode, make sure the **RMC** or **ZDA**
  sentence is switched on (the default GGA alone carries no date).
- Web page, Settings: **Clock from radio's GPS: On**, GPS Out baud to match, Save, restart.
- The Clock dot then shows "gps". GPS receivers send their sentence a little after the second;
  "GPS sentence delay" (default 150 ms) allows for that. If decodes show a steady DT offset,
  lower the delay when DT is mostly negative and raise it when mostly positive.

This opens a second serial port, which an ESP32-S3 has no spare USB channels for; it is meant
for the ESP32-P4.

## Logging

- On the board, every contact is written to two ADIF files, named after the **station callsign**:
  - the **current log** (`AC9HP_log.adi`), which you download and then clear from the Log tab;
  - the **archive** (`AC9HP_archive.adi`), which keeps every contact and is not touched by clearing.
  Typical use: download the current log into your logging program, press "Clear current log",
  and the next download holds only new contacts. "Download full archive" gives you everything.
  "Erase all logs" (Settings, Device; asks twice) is the only thing that deletes the archive.
  Worked-before (per band) is rebuilt from both files at boot, so clearing does not reset it.
- **One log per station:** changing the callsign in Settings switches to that callsign's files
  (a new, empty log the first time); changing it back brings the old log back. The operator
  callsign (OPERATOR) does not change the files: several operators share the station's log.
  `/` in a callsign becomes `-` in file names (`VE3-AC9HP-P_log.adi`). Logs from older firmware
  (`log.adi`, `archive.adi`) become the current callsign's at the first start.
- **Where the log lives:** the board's own memory holds about 2 MB, roughly 2,500 contacts across
  both files. With a **microSD card** (FAT32) in at start-up the logs go on the card instead, in
  the `RigDeck` folder: room for hundreds of thousands of contacts. Contacts logged in the board's
  memory (for example while the card was out) move onto the card at that start. Insert or remove
  the card with the power off: it is only looked for at start-up. Big logs are read a piece at a
  time, never whole, so their size is limited only by the card.
  - On the card a deleted contact is kept for good in `AC9HP_deleted.adi` (the newest 100 are
    listed for Restore), and **Clear current log** keeps the cleared log as
    `AC9HP_deleted_YYYYMMDD_HHMMSS.adi` instead of deleting it. Take the card to a PC to get them.
  - When the store gets close to full the status line says so, with a rough count of contacts left.
- **Editing and deleting contacts:**
  - Web page, Log tab: pick the current log or the archive, find a call, page through (newest
    first), press **Edit** on a contact. Every ADIF field of the contact is listed; change or clear
    any of them, add a field (e.g. STATE), **Save changes**, or **Delete contact**.
  - Touch screen, Logbook: tap a contact. Change call, date, time, frequency (MHz; the band
    follows it), mode (USB, LSB, CW, FT8, FT4, PSK31...), reports, grid, name or comment, then
    **Save**, or **Delete contact** (asks first).
  - A change is made in both files (current log and archive), and worked-before follows it.
    Contacts already sent to a logging program are not changed there.
  - **Recently deleted:** a deleted contact is kept for 7 days in the board's memory (file
    `AC9HP_deleted.adi`), for good on a microSD card. Restore it from
    the Log tab's "Recently deleted" list, or with **Undo delete** in the touch Logbook (newest
    first). It goes back where it was, in time order.
- **Send unsent to logger** (Log tab; touch Logbook "Send unsent (N)"): RigDeck remembers which
  contacts the logging program has received (`sent.txt`). Log in the field with no network, come
  home, press it, and every contact of the current log not sent yet goes to the logger; contacts
  already sent are never sent twice. **Mark all as already sent** is for contacts the logger got
  another way (an ADIF import). With UDP, "sent" means it left the board (the logger does not
  answer); TCP ADIF confirms the connection.
- **Send test to logger** (Log tab) sends a T1EST contact to the logging program set in Settings
  without adding it to RigDeck's log. The line below it shows the last attempt (sent or not,
  where to, when). The first packet to a new address is preceded by a short probe so it is not
  lost while the network looks the address up.
- **Directed CQ:** Settings, Station, **Call CQ as**: plain CQ, CQ DX, CQ POTA, CQ SOTA, CQ WWFF,
  CQ NA, CQ EU, CQ TEST (on the web page any 1-4 letters or 3 digits). The CQ then goes out as
  e.g. "CQ POTA AC9HP EM69"; everything else stays the same.
- **Log QSO** (touch screen next to CQ, web page in the QSO box) logs the contact in progress
  straight away, for when the other station fades out before the exchange finishes, then goes
  back to calling CQ. It needs at least one signal report: one you sent or one you received. With
  neither, nothing is logged. The missing report is left out of the record. A contact that was already logged is not logged twice.
- Optional ADIF fields, set on the Settings tab and written to every contact from then on:
  **OPERATOR** (operator callsign), **MY_SIG_INFO** (for example a park or event reference such
  as US-1234) and, optionally, **MY_SIG** (the programme name, e.g. WWFF; blank by default).
  Empty fields are left out of the log.
- Network (Settings > "Send log to"), one message per finished QSO, retried if the logger is unreachable:
  - **UDP, WSJT-X format**: what WSJT-X sends when it logs (a heartbeat, "QSO Logged" and "Logged
    ADIF"), default port 2237. Use this for programs that listen for WSJT-X (GridTracker, JTAlert,
    Log4OM and others). In Log4OM add a UDP inbound connection of type JT_MESSAGE on that port.
  - **UDP, ADIF text** or **TCP, ADIF text**: the bare ADIF record (TCP adds CR LF).
  The logging PC must be reachable: either joined to the hotspot, or on the network the board joined.

## How it fits together

```
main/            ESP-IDF glue (the only target-specific code)
  ui.cpp         touch interface (LVGL 9), also built on a PC by test/ui_sim.sh
  display.c      starts LCD + touch through Waveshare's board support package
  usb_radio.cpp  USB host: finds the CI-V serial port by probing, opens the codec, audio task
  net.cpp        hotspot (+ optional client), NTP
  web.cpp        HTTP server: page, /api/state, /api/spec, /api/cmd, /log.adi
  web/index.html the control page (no external resources; opened as a file it shows a demo)
components/core/ portable C++, also built and tested on a PC
  app.*          slot timing, receive/transmit hand-off, commands, state JSON, logging
  qso.*          message parsing and the QSO sequencer
  ftx.*  dsp.*   decoder wrapper + SNR estimate, GFSK synthesiser, decimator, waterfall spectrum
  civ.*          CI-V framing and IC-7100 control (frequency, USB-D, PTT, meters)
  cwx.*          FT-CW: Morse framing, keyed-tone synthesiser, multi-frequency decoder
  nmea.*         GPS time sentences
  psk31.*        BPSK31 modem      adif.* netlog.*  log formats and network senders
components/ft8_lib/  vendored github.com/kgoba/ft8_lib (MIT) incl. KISS FFT (BSD-3-Clause)
test/            host tests: sh test/build.sh
```

Audio runs at 48 kHz 16-bit over USB, decimated to 12 kHz for decoding. Decoding starts 13.6 s
into the slot (6.0 s for FT4) so the reply is ready for the next slot; if the decoder runs past
the boundary the transmission starts late, part-way through the message, up to 2 s in (FT8).
The page shows the decode time. If it is regularly above ~1400 ms on your board, lower
`maxCandidates` in `FtxMonitor::decode` (ftx.h).

## Known limits

- SNR reports are an estimate from the waterfall (within about 1 dB of the simulated truth for
  FT8 in the tests, but not WSJT-X's algorithm).
- The decoder is ft8_lib's single-pass decoder: no signal subtraction or a-priori decoding, so it
  finds fewer weak signals than WSJT-X on a crowded band.
- Standard messages only in the sequencer; contest and DXpedition exchanges are displayed, not worked.
- PSK31 is keyboard-to-keyboard with manual logging; one over is sent per Send.
- FTX-1 and QMX command formats come from their CAT manuals; USB IDs and audio formats from
  published descriptions. Real radios may differ in details (the probe log in the monitor output
  will show what was found).
- The web page has no login of its own; the hotspot password is the access control.
- Settings live on SPIFFS; the log on SPIFFS or the microSD card. A new contact is appended; an
  edit, delete or restore rewrites the file through a temporary copy, so a large log on the card
  takes a moment (not allowed while transmitting). The card is only looked for at start-up.

## Licence

RigDeck's own code: MIT licence, see [LICENSE](LICENSE). Bundled third-party code keeps its own licence;
see [THIRD_PARTY.md](THIRD_PARTY.md).
