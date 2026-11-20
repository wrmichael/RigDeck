#pragma once
#include "app.h"
void net_start(App& app);          // Wi-Fi access point (+ optional station), NTP
void web_start(App& app);
void wifichip_start(App& app);     // ESP32-P4 only: reads the Wi-Fi chip's firmware version          // HTTP server with the control page and JSON API
void usbradio_start(App& app);
void gps_module_start(App& app);   // optional GPS module on the board header (UART + PPS)
extern "C" void display_start(void);   // LCD + touch + LVGL, then builds the on-screen interface (ui.cpp)     // USB host: CI-V serial port and audio codec of the IC-7100
