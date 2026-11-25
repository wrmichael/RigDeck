// Detailed APRS map on the microSD card: loading at start-up and the web page's upload (see mapfile.cpp).
#pragma once
#include "app.h"
#include "esp_http_server.h"
void mapfile_start(App& app, bool haveCard);
void mapfile_register(httpd_handle_t srv);
