# espressif/usb 1.6.1, patched for RigDeck

A copy of the USB Host Library (espressif/usb 1.6.1, Apache-2.0) from the component registry. A component in the
project's components/ folder takes the place of the managed one.

Patch (src/hcd_dwc.c, marked "RigDeck patch"): `usb_host_config_t.fsls_only` works with ESP-IDF 6.1, whose USB DWC
HAL has no FS/LS-only option. The port sets the core's FSLSSupp bit (HCFG) before every bus reset, as newer ESP-IDF
HALs do in `usb_dwc_hal_port_toggle_reset`, and keeps the setting through port recovery. The ESP32-P4's high-speed
port then runs at full speed, so the full-speed serial and audio chips behind a radio's high-speed USB hub
(FTX-1) are reachable without a transaction translator.
