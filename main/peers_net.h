// RigDecks on the same home network find each other (peers_net.cpp).
#pragma once
#include "app.h"
#include "esp_http_server.h"
void peers_start(App& app);
void peers_register(httpd_handle_t srv);
