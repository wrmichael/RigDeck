// Detailed APRS map on the microSD card: loading at start-up and the web page's upload (see mapfile.cpp).
#pragma once
#include "app.h"
#include "esp_http_server.h"
void mapfile_start(App& app, bool haveCard);
void mapfile_register(httpd_handle_t srv);
#include <functional>
#include <string>
// HTTPS GET (certificate bundle); each piece of the body goes to sink (false = stop). Returns "" or why it failed.
std::string web_get(const std::string& url, const std::function<bool(const char*, int, int64_t)>& sink);
// Every 6 hours on a home network: which releases the RigDeck site has (App::setReleases, "update available").
void updcheck_start(App& app);
