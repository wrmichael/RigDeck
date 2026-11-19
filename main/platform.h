#pragma once
#include "app.h"
void net_start(App& app);          // Wi-Fi access point (+ optional station), NTP
void web_start(App& app);          // HTTP server with the control page and JSON API
void usbradio_start(App& app);
extern "C" void display_start(void);   // LCD + touch + LVGL, then builds the on-screen interface (ui.cpp)     // USB host: CI-V serial port and audio codec of the IC-7100
