// Wi-Fi: always an access point (the "hotspot" you connect your phone or laptop to), and optionally
// also a client on an existing network so NTP and a logging PC are reachable.
#include "platform.h"
#include "clock.h"
#include <cstring>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"

static const char* TAG = "net";
static App* s_app;
static bool s_sta = false;
static char s_staIp[20] = "";

static void publish() {
    std::string s = "AP \"" + s_app->cfg.apSsid + "\" 192.168.4.1";
    if (s_sta) s += std::string(", LAN ") + (s_staIp[0] ? s_staIp : "connecting...");
    s_app->setNetInfo(s);
}

static void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_staIp[0] = 0;
        publish();
        esp_wifi_connect();                         // keep trying; the access point stays up meanwhile
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        snprintf(s_staIp, sizeof s_staIp, IPSTR, IP2STR(&e->ip_info.ip));
        publish();
    }
}

static void onNtp(struct timeval* tv) {
    clk::setUtcMs((int64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000, "ntp");
    s_app->ntpSynced = true;
    ESP_LOGI(TAG, "clock set from NTP");
}

void net_start(App& app) {
    s_app = &app;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    s_sta = !app.cfg.staSsid.empty();
    if (s_sta) esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr, nullptr);

    wifi_config_t ap = {};
    strlcpy((char*)ap.ap.ssid, app.cfg.apSsid.c_str(), sizeof ap.ap.ssid);
    ap.ap.ssid_len = (uint8_t)strlen((char*)ap.ap.ssid);
    strlcpy((char*)ap.ap.password, app.cfg.apPass.c_str(), sizeof ap.ap.password);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = app.cfg.apPass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(s_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (s_sta) {
        wifi_config_t sta = {};
        strlcpy((char*)sta.sta.ssid, app.cfg.staSsid.c_str(), sizeof sta.sta.ssid);
        strlcpy((char*)sta.sta.password, app.cfg.staPass.c_str(), sizeof sta.sta.password);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    publish();

    if (s_sta) {                                    // internet time when we have a route to it
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        sntp_set_time_sync_notification_cb(onNtp);
        esp_sntp_init();
    }
}
