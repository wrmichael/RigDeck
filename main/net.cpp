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
#include "esp_timer.h"
#include <string>

static const char* TAG = "net";
static App* s_app;
static esp_netif_t* s_staNetif = nullptr;
static bool s_want = false;                          // the home network should be joined
static bool s_sntp = false;
static int s_retry = 0;
static App::StaStatus s_st;
static esp_timer_handle_t s_retryTimer = nullptr, s_rssiTimer = nullptr;

static void publish() {
    std::string s = "AP \"" + s_app->cfg.apSsid + "\" 192.168.4.1";
    if (s_st.state != "off") s += ", home network " + (s_st.ip.empty() ? s_st.state : s_st.ip);
    s_app->setNetInfo(s);
    s_app->setStaStatus(s_st);
}

static void setState(const char* state, const std::string& reason = "") {
    s_st.state = state;
    s_st.reason = reason;
    if (strcmp(state, "connected")) { s_st.ip.clear(); s_st.rssi = 0; }
    publish();
}

// What a disconnect reason means for the person holding the board (wifi_err_reason_t numbers).
static std::string reasonText(int r) {
    switch (r) {
    case 201: return "network not found: check the name; it must be a 2.4 GHz network";
    case 210: case 211: return "network found but its security type is not supported";
    case 212: return "signal too weak";
    case 15: case 202: case 204: return "wrong password?";
    case 2: case 203: case 205: return "the router refused the connection";
    case 200: return "signal lost";
    default: return "disconnected (reason " + std::to_string(r) + ")";
    }
}

static void staConnectNow() {
    if (!s_want) return;
    setState("connecting");
    esp_wifi_connect();
}
static void onRetryTimer(void*) { staConnectNow(); }

static void onRssiTimer(void*) {
    if (s_st.state != "connected") return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.rssi != s_st.rssi) { s_st.rssi = ap.rssi; publish(); }
}

static void startSntp() {
    if (s_sntp) return;
    s_sntp = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb([](struct timeval* tv) {
        clk::setUtcMs((int64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000, "ntp");
        s_app->ntpSynced = true;
        ESP_LOGI(TAG, "clock set from NTP");
    });
    esp_sntp_init();
}

// Join (or re-join) the home network with the saved name and password. Creates the station
// interface the first time, so this works without a restart.
static void staJoin() {
    const Settings& c = s_app->cfg;
    if (c.staSsid.empty()) { s_want = false; setState("off"); return; }
    if (!s_staNetif) s_staNetif = esp_netif_create_default_wifi_sta();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t sta = {};
    strlcpy((char*)sta.sta.ssid, c.staSsid.c_str(), sizeof sta.sta.ssid);
    strlcpy((char*)sta.sta.password, c.staPass.c_str(), sizeof sta.sta.password);
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;        // strongest access point with this name
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_want = true;
    s_retry = 0;
    s_st.ssid = c.staSsid;
    esp_timer_stop(s_retryTimer);
    ESP_LOGI(TAG, "joining \"%s\"", c.staSsid.c_str());
    staConnectNow();
    startSntp();
}

static void staLeave() {
    s_want = false;
    esp_timer_stop(s_retryTimer);
    if (s_staNetif) esp_wifi_disconnect();
    setState(s_app->cfg.staSsid.empty() ? "off" : "disconnected");
}

static void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want) staConnectNow();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        setState("getting address");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t* e = (const wifi_event_sta_disconnected_t*)data;
        int r = e ? e->reason : 0;
        if (!s_want) { setState(s_app->cfg.staSsid.empty() ? "off" : "disconnected"); return; }
        // Keep trying, a little slower each time (2, 4, 8 ... 60 s); the hotspot stays up meanwhile.
        int delayS = s_retry < 5 ? (2 << s_retry) : 60;
        s_retry++;
        std::string why = reasonText(r);
        ESP_LOGW(TAG, "home network: %s; retry in %d s", why.c_str(), delayS);
        setState("retrying", why);
        esp_timer_stop(s_retryTimer);
        esp_timer_start_once(s_retryTimer, (uint64_t)delayS * 1000000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        char ip[20];
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_st.state = "connected";
        s_st.reason.clear();
        s_st.ip = ip;
        ESP_LOGI(TAG, "home network connected, address %s", ip);
        publish();
        onRssiTimer(nullptr);
    }
}

void net_start(App& app) {
    s_app = &app;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    s_want = !app.cfg.staSsid.empty();
    if (s_want) s_staNetif = esp_netif_create_default_wifi_sta();
    esp_timer_create_args_t ta = {};
    ta.callback = onRetryTimer;
    ta.name = "sta-retry";
    esp_timer_create(&ta, &s_retryTimer);
    ta.callback = onRssiTimer;
    ta.name = "sta-rssi";
    esp_timer_create(&ta, &s_rssiTimer);
    esp_timer_start_periodic(s_rssiTimer, 10 * 1000000ULL);
    app.wifiControl = [](bool connect) { if (connect) staJoin(); else staLeave(); };

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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_st.state = "off";
    publish();
    if (s_want) staJoin();                          // internet time and the logging PC when we have a route
}
