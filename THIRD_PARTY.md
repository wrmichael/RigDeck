# Third-party code in RigDeck

| Part | Where | Licence | Notes |
|---|---|---|---|
| ft8_lib by Karlis Goba | `components/ft8_lib` | MIT (`components/ft8_lib/LICENSE`) | commit 9fec6ca, unmodified subset |
| KISS FFT by Mark Borgerding | `components/ft8_lib/fft` | BSD-3-Clause | |
| ESP-Hosted by Espressif | `components/espressif__esp_hosted` | Apache-2.0 (`components/espressif__esp_hosted/LICENSE`) | version 3.1.0, **modified** (see below) |
| ESP-IDF, USB host, UAC, CDC-ACM, esp_wifi_remote | fetched at build time | Apache-2.0 | Espressif component registry |
| LVGL 9.5 | fetched at build time | MIT | |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-7B BSP | fetched at build time | Apache-2.0 | |
| PSK31 varicode table | `components/core/varicode.inc` | public alphabet by G3PLX | |

## Change to ESP-Hosted

`host/mcu/eh_host_mcu_transport/src/eh_host_mcu_transport_init_event.c`: when the ESP32-C6
co-processor does not report its SDIO mode (the older co-processor firmware shipped on the Waveshare
boards), the host now assumes streaming mode instead of falling back to packet mode. Without this
the Wi-Fi drops every multi-frame SDIO read and the hotspot does not work. The changed lines are
marked "IC7100-FT8 local patch". This copy takes precedence over the registry version because it
sits in the project's `components/` folder.
