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
#include "esp_heap_caps.h"
#include "psa/crypto.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

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

    // Internal RAM on purpose: flash writes from an external-RAM buffer go through extra copying.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(kChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return reply(req, false, "Not enough memory");
    struct Free { uint8_t* p; ~Free() { heap_caps_free(p); } } fr{buf};

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

    // SHA-256 of what arrived (all but the appended 32-byte hash), to tell a damaged transfer from a
    // damaged flash write if the final check fails.
    const bool hashed = buf[23] == 1;
    const size_t hashLen = hashed && appLen > 32 ? appLen - 32 : appLen;
    uint8_t tail[32] = {};
    psa_crypto_init();
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&sha, PSA_ALG_SHA_256);
    size_t sent = 0;
    int lastPct = -1;
    for (;;) {
        if (sent < hashLen) psa_hash_update(&sha, buf, std::min(got, hashLen - sent));
        for (size_t i = 0; i < got; i++)
            if (sent + i >= hashLen && sent + i - hashLen < 32) tail[sent + i - hashLen] = buf[i];
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
    uint8_t got_sha[32];
    size_t shaLen = 0;
    psa_hash_finish(&sha, got_sha, sizeof got_sha, &shaLen);
    bool rxOk = !hashed || memcmp(got_sha, tail, 32) == 0;
    ab.on = false;
    esp_log_level_set("esp_image", ESP_LOG_INFO);         // say which check fails, if one does
    e = esp_ota_end(h);                               // checks the whole image (segments and SHA-256)
    esp_log_level_set("esp_image", ESP_LOG_WARN);
    if (e != ESP_OK) {
        std::string why = rxOk ? "the file arrived intact" : "the file arrived damaged (SHA-256 mismatch)";
        if (rxOk) {                                    // read the slot back: did flash keep what was written?
            psa_hash_operation_t rb = PSA_HASH_OPERATION_INIT;
            psa_hash_setup(&rb, PSA_ALG_SHA_256);
            bool rdOk = true;
            for (size_t off = 0; off < hashLen && rdOk; off += kChunk) {
                size_t n = std::min(kChunk, hashLen - off);
                rdOk = esp_partition_read(part, off, buf, n) == ESP_OK;
                psa_hash_update(&rb, buf, n);
            }
            uint8_t fl[32];
            psa_hash_finish(&rb, fl, sizeof fl, &shaLen);
            why += rdOk && memcmp(fl, got_sha, 32) == 0 ? ", flash matches it" : ", but flash does not match what was written";
        }
        ESP_LOGE(TAG, "update check failed: %s", why.c_str());
        return reply(req, false, std::string("The image did not pass the final check (") + esp_err_to_name(e) + "; " + why +
                                     "). RigDeck keeps its current firmware.");
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK)
        return reply(req, false, std::string("Could not switch to the new firmware (") + esp_err_to_name(e) + ").");
    xTaskCreate(restartTask, "otarst", 3072, nullptr, 2, nullptr);
    return reply(req, true, "RigDeck " + ver + " installed. Restarting now; reconnect in about 30 seconds.");
}


// ---- Chunked upload with a CRC per piece (used by the web page since 0.9.50) -------------------
// POST /api/otachunk?off=N&total=T&crc=C with up to 64 kB of the app image as the body. Each piece is
// checked with CRC-32 as soon as it has arrived; a damaged piece is not written and the page sends it
// again. The pieces must come in order; the last one finishes the update. The page already strips the
// first 64 kB of a complete flasher image, so `total` is the app image size.
struct ChunkSession {
    bool active = false;
    esp_ota_handle_t h = 0;
    const esp_partition_t* part = nullptr;
    size_t total = 0, written = 0, hashLen = 0;
    bool hashed = false;
    uint8_t tail[32] = {};
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    std::string ver;
    int bad = 0;                                       // pieces that arrived damaged (and were sent again)
    int64_t lastUs = 0;
};
static ChunkSession s_cs;
static const size_t kPiece = 65536;

static void csAbort() {
    if (s_cs.active) { esp_ota_abort(s_cs.h); psa_hash_abort(&s_cs.sha); }
    s_cs = ChunkSession();
}

static bool queryNum(httpd_req_t* req, const char* key, uint32_t& out) {
    char q[128], v[24];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return false;
    out = (uint32_t)strtoul(v, nullptr, 0);
    return true;
}

static esp_err_t chunk(httpd_req_t* req) {
    uint32_t off = 0, total = 0, crc = 0;
    if (!queryNum(req, "off", off) || !queryNum(req, "total", total) || !queryNum(req, "crc", crc))
        return reply(req, false, "bad request");
    size_t n = req->content_len;
    if (n == 0 || n > kPiece || off + n > total) return reply(req, false, "bad piece size");
    if (s_busy.exchange(true)) return reply(req, false, "An update is already running");
    struct Done { ~Done() { s_busy = false; } } done;
    if (s_cs.active && esp_timer_get_time() - s_cs.lastUs > 60000000LL) csAbort();   // stale session

    static uint8_t* piece = nullptr;                   // 64 kB in external RAM, kept for later updates
    if (!piece) piece = (uint8_t*)heap_caps_malloc(kPiece, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!piece) return reply(req, false, "Not enough memory");
    if (!recvAll(req, piece, n)) return reply(req, false, "piece interrupted");
    uint32_t c = esp_rom_crc32_le(0, piece, n);
    if (c != crc) {
        s_cs.bad++;
        ESP_LOGW(TAG, "piece at %u (%u bytes) arrived damaged: CRC %08x, expected %08x", (unsigned)off, (unsigned)n,
                 (unsigned)c, (unsigned)crc);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"retry\":true,\"err\":\"damaged piece\"}");
    }

    if (off == 0) {                                    // first piece: check it and start
        csAbort();
        std::string bad = checkHeader(piece, s_cs.ver);
        if (!bad.empty()) return reply(req, false, bad + " Nothing was changed.");
        s_cs.part = esp_ota_get_next_update_partition(nullptr);
        if (!s_cs.part)
            return reply(req, false, "This board's flash layout has no update slot yet. Install this version once "
                                     "with the browser flasher over USB; after that, updates work from here.");
        if (total > s_cs.part->size) return reply(req, false, "That image is too big for the update slot.");
        ESP_LOGI(TAG, "update to %s, %u bytes in pieces, into %s", s_cs.ver.c_str(), (unsigned)total, s_cs.part->label);
        s_app->command("halt", {});
        s_app->setOtaStatus("Preparing (erasing the update slot)...");
        if (esp_ota_begin(s_cs.part, OTA_WITH_SEQUENTIAL_WRITES, &s_cs.h) != ESP_OK)
            return reply(req, false, "Could not start the update. Nothing was changed.");
        s_cs.active = true;
        s_cs.total = total;
        s_cs.hashed = piece[23] == 1;
        s_cs.hashLen = s_cs.hashed && total > 32 ? total - 32 : total;
        psa_crypto_init();
        psa_hash_setup(&s_cs.sha, PSA_ALG_SHA_256);
    }
    if (!s_cs.active || total != s_cs.total) return reply(req, false, "No update in progress. Start again.");
    s_cs.lastUs = esp_timer_get_time();
    if (off + n <= s_cs.written) return reply(req, true, "already have it");      // a resend of a piece we kept
    if (off != s_cs.written) return reply(req, false, "pieces out of order. Start again.");

    size_t hn = off < s_cs.hashLen ? std::min(n, s_cs.hashLen - (size_t)off) : 0;
    if (hn) psa_hash_update(&s_cs.sha, piece, hn);
    for (size_t i = hn; i < n; i++)
        if (off + i >= s_cs.hashLen && off + i - s_cs.hashLen < 32) s_cs.tail[off + i - s_cs.hashLen] = piece[i];
    if (esp_ota_write(s_cs.h, piece, n) != ESP_OK) { csAbort(); return reply(req, false, "Writing failed. RigDeck keeps its current firmware."); }
    s_cs.written += n;
    s_app->setOtaStatus("Updating RigDeck: " + std::to_string((int)(s_cs.written * 100 / s_cs.total)) + " %");
    if (s_cs.written < s_cs.total) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    // Last piece: whole-image checks, switch, restart.
    uint8_t got[32];
    size_t shaLen = 0;
    psa_hash_finish(&s_cs.sha, got, sizeof got, &shaLen);
    bool rxOk = !s_cs.hashed || memcmp(got, s_cs.tail, 32) == 0;
    esp_err_t e = esp_ota_end(s_cs.h);
    const esp_partition_t* part = s_cs.part;
    std::string ver = s_cs.ver;
    int bad = s_cs.bad;
    s_cs = ChunkSession();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "update check failed, sha of pieces %s, %d piece(s) resent", rxOk ? "ok" : "MISMATCH", bad);
        return reply(req, false, std::string("The image did not pass the final check (") + esp_err_to_name(e) +
                                     (rxOk ? "; every piece arrived intact" : "; pieces intact but the whole does not match") +
                                     "). RigDeck keeps its current firmware.");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) return reply(req, false, "Could not switch to the new firmware.");
    ESP_LOGI(TAG, "update to %s complete, %d piece(s) had to be resent", ver.c_str(), bad);
    xTaskCreate(restartTask, "otarst", 3072, nullptr, 2, nullptr);
    return reply(req, true, "RigDeck " + ver + " installed" + (bad ? " (" + std::to_string(bad) + " damaged piece(s) were resent)" : std::string()) +
                                ". Restarting now; reconnect in about 30 seconds.");
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
    static const httpd_uri_t c = {"/api/otachunk", HTTP_POST, chunk, nullptr};
    httpd_register_uri_handler(srv, &c);
}
