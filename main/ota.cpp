// RigDeck firmware update over Wi-Fi (web page: Device > Update RigDeck).
//
// Flash layout (partitions.csv): "factory" holds the image written over USB (browser flasher or
// idf.py flash); ota_0 / ota_1 take turns holding images uploaded from the web page. The board keeps
// running its current firmware until the new image has arrived complete and been checked; only then
// does it switch and restart. A new image that does not reach "running normally" (crash or reset in
// its first minute) is rolled back by the bootloader to the previous one.
//
// Accepted files: the app image (build\rigdeck.bin) or the complete image from the flasher page
// (rigdeck-x.y.z.bin, which has the app at 0x10000: the first 64 kB are skipped).
#include "platform.h"
#include <atomic>
#include <cstring>
#include <string>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"

static const char* TAG = "ota";
static App* s_app;
static std::atomic<bool> s_busy{false};
static const size_t kChunk = 4096;
static const size_t kMergedAppOffset = 0x10000;       // where the app sits inside the complete image
static const uint16_t kChipP4 = 18;                   // esp_chip_id_t ESP_CHIP_ID_ESP32P4

static esp_err_t reply(httpd_req_t* req, bool ok, const std::string& msg) {
    ESP_LOGI(TAG, "%s", msg.c_str());
    if (s_app) s_app->setOtaStatus(msg);
    std::string js = std::string("{\"ok\":") + (ok ? "true" : "false") + (ok ? ",\"msg\":" : ",\"err\":") + "\"";
    for (char c : msg) { if (c == '"' || c == '\\') js += '\\'; js += c; }
    js += "\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, js.data(), js.size());
}

static bool recvAll(httpd_req_t* req, uint8_t* p, size_t n) {
    int timeouts = 0;
    while (n) {
        int r = httpd_req_recv(req, (char*)p, n);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) { if (++timeouts > 20) return false; continue; }
        if (r <= 0) return false;
        timeouts = 0;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static void restartTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));                  // let the reply reach the browser
    s_app->rebootReq = true;
    vTaskDelete(nullptr);
}

// An image is good when it starts with the image magic, is for the ESP32-P4 and carries the
// application description of a RigDeck build.
static std::string checkHeader(const uint8_t* b, std::string& ver) {
    if (b[0] != ESP_IMAGE_HEADER_MAGIC) return "That file is not a RigDeck firmware image (rigdeck-x.y.z.bin).";
    uint16_t chip = b[12] | (b[13] << 8);
    if (chip != kChipP4) return "That firmware is for a different chip (not the ESP32-P4).";
    const esp_app_desc_t* d = (const esp_app_desc_t*)(b + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) return "That file is not a firmware image.";
    char proj[33] = {}, v[33] = {};
    memcpy(proj, d->project_name, 32);
    memcpy(v, d->version, 32);
    if (strcmp(proj, "rigdeck") != 0) return std::string("That is firmware for something else (") + proj + "), not RigDeck.";
    ver = v;
    return "";
}

static esp_err_t upload(httpd_req_t* req) {
    if (s_busy.exchange(true)) return reply(req, false, "An update is already running");
    struct Done { ~Done() { s_busy = false; } } done;
    size_t len = req->content_len;
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part)
        return reply(req, false, "This board's flash layout has no update slot yet. Install this version once "
                                 "with the browser flasher over USB; after that, updates work from here.");
    if (len < 256 * 1024 || len > kMergedAppOffset + part->size)
        return reply(req, false, "That file is not RigDeck firmware (wrong size). Use rigdeck-x.y.z.bin.");

    uint8_t* buf = (uint8_t*)malloc(kChunk);
    if (!buf) return reply(req, false, "Not enough memory");
    struct Free { uint8_t* p; ~Free() { free(p); } } fr{buf};

    size_t got = std::min(kChunk, len);
    if (!recvAll(req, buf, got)) return reply(req, false, "Upload interrupted. Nothing was changed.");
    size_t appLen = len;
    if (buf[0] != ESP_IMAGE_HEADER_MAGIC) {           // complete image from the flasher page: skip to the app
        if (len <= kMergedAppOffset + kChunk) return reply(req, false, "That file is not RigDeck firmware.");
        size_t skip = kMergedAppOffset - got;
        while (skip) {
            size_t n = std::min(kChunk, skip);
            if (!recvAll(req, buf, n)) return reply(req, false, "Upload interrupted. Nothing was changed.");
            skip -= n;
        }
        appLen = len - kMergedAppOffset;
        got = std::min(kChunk, appLen);
        if (!recvAll(req, buf, got)) return reply(req, false, "Upload interrupted. Nothing was changed.");
    }
    if (appLen > part->size) return reply(req, false, "That image is too big for the update slot.");
    std::string ver;
    std::string bad = checkHeader(buf, ver);
    if (!bad.empty()) return reply(req, false, bad + " Nothing was changed.");
    ESP_LOGI(TAG, "update to %s, %u bytes, into %s", ver.c_str(), (unsigned)appLen, part->label);

    s_app->command("halt", {});                       // stop transmitting: the update takes a minute
    s_app->setOtaStatus("Preparing (erasing the update slot)...");
    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) return reply(req, false, std::string("Could not start the update (") + esp_err_to_name(e) + "). Nothing was changed.");
    struct Abort { esp_ota_handle_t& h; bool on = true; ~Abort() { if (on) esp_ota_abort(h); } } ab{h};

    size_t sent = 0;
    int lastPct = -1;
    for (;;) {
        e = esp_ota_write(h, buf, got);
        if (e != ESP_OK)
            return reply(req, false, std::string("Writing failed (") + esp_err_to_name(e) + "). RigDeck keeps its current firmware.");
        sent += got;
        int pct = (int)(sent * 100 / appLen);
        if (pct / 10 != lastPct / 10) {
            lastPct = pct;
            s_app->setOtaStatus("Updating RigDeck: " + std::to_string(pct) + " %");
        }
        if (sent >= appLen) break;
        got = std::min(kChunk, appLen - sent);
        if (!recvAll(req, buf, got))
            return reply(req, false, "Upload interrupted. RigDeck keeps its current firmware.");
    }
    ab.on = false;
    e = esp_ota_end(h);                               // checks the whole image (segments and SHA-256)
    if (e != ESP_OK)
        return reply(req, false, std::string("The image is damaged or incomplete (") + esp_err_to_name(e) +
                                     "). RigDeck keeps its current firmware.");
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK)
        return reply(req, false, std::string("Could not switch to the new firmware (") + esp_err_to_name(e) + ").");
    xTaskCreate(restartTask, "otarst", 3072, nullptr, 2, nullptr);
    return reply(req, true, "RigDeck " + ver + " installed. Restarting now; reconnect in about 30 seconds.");
}

// A freshly updated image starts "on trial". Once it has run for a minute it is kept; if it crashes
// or resets before that, the bootloader goes back to the previous firmware.
static void validTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(60000));
    esp_ota_img_states_t st;
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed, kept");
        if (s_app) s_app->setOtaStatus("Update finished: this firmware is now kept.");
    }
    vTaskDelete(nullptr);
}

void ota_start(App& app) {
    s_app = &app;
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    std::string info = run ? run->label : "?";
    if (!next) info += " (no update slot: install once over USB to enable updates from the web page)";
    app.otaInfo = info;
    ESP_LOGI(TAG, "running from %s", info.c_str());
    xTaskCreate(validTask, "otaok", 3072, nullptr, 2, nullptr);
}

void ota_register(httpd_handle_t srv) {
    static const httpd_uri_t u = {"/api/ota", HTTP_POST, upload, nullptr};
    httpd_register_uri_handler(srv, &u);
}
