// The board's Wi-Fi chip (ESP32-C6 running ESP-Hosted): report its firmware version, and update
// that firmware from a file uploaded on the web page (Settings > Wi-Fi chip).
//
// The update goes over the same SDIO link RigDeck already uses for Wi-Fi: begin (the chip erases
// its spare firmware slot), write the image in pieces, end (the chip checks the image), then
// activate on firmware 2.6 and later. The chip keeps running its current firmware until the new one
// has arrived complete and checked, so a failed or cut-off upload changes nothing.
#include "platform.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "eh_host_api_types.h"

// ESP-Hosted host API (eh_host_sys.h, eh_host_cp_ota.h). Declared here because those headers pull
// in C-only wire definitions (_Static_assert) that do not compile as C++.
extern "C" {
esp_err_t eh_host_sys_get_cp_fw_version(eh_host_coprocessor_fwver_t* ver_info);
esp_err_t eh_host_cp_ota_begin(void);
esp_err_t eh_host_cp_ota_write(const uint8_t* ota_data, uint32_t ota_data_len);
esp_err_t eh_host_cp_ota_end(void);
esp_err_t eh_host_cp_ota_activate(void);
}

static const char* TAG = "wifichip";
static App* s_app;
static std::atomic<bool> s_busy{false};
static std::atomic<bool> s_canActivate{false};       // firmware 2.6.0 or later has a separate "activate" step
static const size_t kChunk = 1400;                    // per write; fits every ESP-Hosted version's limit
static const size_t kMaxImage = 0x1C0000;             // one firmware slot on the chip (1.75 MB)

static void versionTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(6000));                  // after Wi-Fi is up
    eh_host_coprocessor_fwver_t v = {};
    esp_err_t e = eh_host_sys_get_cp_fw_version(&v);
    std::string ver;
    if (e == ESP_OK && (v.major1 || v.minor1 || v.patch1)) {
        char b[32];
        snprintf(b, sizeof b, "%u.%u.%u", (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
        ver = b;
        s_canActivate = v.major1 > 2 || (v.major1 == 2 && v.minor1 > 5);
    } else {
        ver = "old (does not report a version)";
    }
    ESP_LOGI(TAG, "Wi-Fi chip firmware: %s", ver.c_str());
    s_app->setWifiChip(ver, "");
    vTaskDelete(nullptr);
}

void wifichip_start(App& app) {
    s_app = &app;
    xTaskCreate(versionTask, "wchipver", 4096, nullptr, 2, nullptr);
}

static esp_err_t reply(httpd_req_t* req, bool ok, const std::string& msg) {
    s_app->setWifiChipUpd(msg);
    ESP_LOGI(TAG, "%s", msg.c_str());
    std::string js = std::string("{\"ok\":") + (ok ? "true" : "false") + (ok ? ",\"msg\":" : ",\"err\":") + "\"";
    for (char c : msg) { if (c == '"' || c == '\\') js += '\\'; js += c; }
    js += "\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, js.data(), js.size());
}

static void restartTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(3000));                  // let the reply go out and the chip finish restarting
    s_app->rebootReq = true;
    vTaskDelete(nullptr);
}

// Reads exactly n bytes of the request body (or fails).
static bool recvAll(httpd_req_t* req, uint8_t* p, size_t n) {
    while (n) {
        int r = httpd_req_recv(req, (char*)p, n);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static esp_err_t upload(httpd_req_t* req) {
    if (s_busy.exchange(true)) return reply(req, false, "An update is already running");
    struct Done { ~Done() { s_busy = false; } } done;
    size_t len = req->content_len;
    if (len < 64 * 1024 || len > kMaxImage)
        return reply(req, false, "That file is not Wi-Fi chip firmware (wrong size). Use rigdeck-wifi-chip.bin.");

    uint8_t* buf = (uint8_t*)malloc(kChunk);
    if (!buf) return reply(req, false, "Not enough memory");
    struct Free { uint8_t* p; ~Free() { free(p); } } fr{buf};

    // First piece: check it is an ESP32-C6 application image before touching the chip.
    // Image header: magic 0xE9 at 0, chip id at 12 (13 = ESP32-C6); app description at 32
    // (magic 0xABCD5432, version at +16, project name at +48).
    size_t first = std::min(kChunk, len);
    if (!recvAll(req, buf, first)) return reply(req, false, "Upload interrupted");
    uint32_t descMagic = buf[32] | (buf[33] << 8) | (buf[34] << 16) | ((uint32_t)buf[35] << 24);
    uint16_t chip = buf[12] | (buf[13] << 8);
    if (buf[0] != 0xE9 || descMagic != 0xABCD5432)
        return reply(req, false, "That file is not a firmware image. Use rigdeck-wifi-chip.bin.");
    if (chip != 13)
        return reply(req, false, "That firmware is for a different chip (not the ESP32-C6). Use rigdeck-wifi-chip.bin.");
    char newVer[33] = {}, project[33] = {};
    memcpy(newVer, buf + 32 + 16, 32);
    memcpy(project, buf + 32 + 48, 32);
    ESP_LOGI(TAG, "update: %s %s, %u bytes", project, newVer, (unsigned)len);

    s_app->command("halt", {});                       // no transmitting while the radio link may hiccup
    s_app->setWifiChipUpd("Preparing the Wi-Fi chip (erasing)...");
    esp_err_t e = eh_host_cp_ota_begin();
    if (e != ESP_OK)
        return reply(req, false, std::string("The Wi-Fi chip refused the update (") + esp_err_to_name(e) +
                                     "). Its firmware may be too old to accept updates this way. Nothing was changed.");

    size_t sent = 0, have = first;
    int lastPct = -1;
    for (;;) {
        e = eh_host_cp_ota_write(buf, (uint32_t)have);
        if (e != ESP_OK) {
            char m[160];
            snprintf(m, sizeof m, "Writing failed at %u of %u bytes (%s). The Wi-Fi chip keeps its old firmware.",
                     (unsigned)sent, (unsigned)len, esp_err_to_name(e));
            return reply(req, false, m);
        }
        sent += have;
        int pct = (int)(sent * 100 / len);
        if (pct / 10 != lastPct / 10) {
            lastPct = pct;
            s_app->setWifiChipUpd("Updating the Wi-Fi chip: " + std::to_string(pct) + " %");
        }
        if (sent >= len) break;
        have = std::min(kChunk, len - sent);
        if (!recvAll(req, buf, have))
            return reply(req, false, "Upload interrupted. The Wi-Fi chip keeps its old firmware.");
    }
    e = eh_host_cp_ota_end();
    if (e != ESP_OK)
        return reply(req, false, std::string("The Wi-Fi chip rejected the image (") + esp_err_to_name(e) +
                                     "). It keeps its old firmware.");
    if (s_canActivate) {
        e = eh_host_cp_ota_activate();
        if (e != ESP_OK)
            return reply(req, false, std::string("The image arrived but could not be switched on (") + esp_err_to_name(e) + ").");
    }
    xTaskCreate(restartTask, "wchiprst", 3072, nullptr, 2, nullptr);
    return reply(req, true, std::string("Wi-Fi chip firmware ") + newVer +
                                " installed. RigDeck restarts now; reconnect in about 30 seconds.");
}

void wifichip_register(httpd_handle_t srv) {
    static const httpd_uri_t u = {"/api/wifichip", HTTP_POST, upload, nullptr};
    httpd_register_uri_handler(srv, &u);
}
