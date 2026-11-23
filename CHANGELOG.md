# Changes

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
