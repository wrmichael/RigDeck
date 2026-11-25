// Detailed APRS map on the microSD card (RigDeck/map.rdm, built by tools/make_map.py --detail and published on
// the GitHub site). The web page downloads it from GitHub and sends it here in pieces; it is checked whole,
// then kept in PSRAM for the touch screen. Without it the built-in outlines are used.
#include "mapfile.h"
#include "platform.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "adif.h"
#include "mapdata.h"
#include "sdcard.h"
#if CONFIG_IC7100_DISPLAY
#include "ui.h"
#endif

static const char* TAG = "map";
static const char* kPath = SDCARD_LOG_DIR "/map.rdm";
static const char* kTmp = SDCARD_LOG_DIR "/map.tmp";
static const size_t kMax = 16u << 20;               // the published file is about 3.5 MB
static const size_t kPiece = 65536;
static App* s_app = nullptr;
static std::atomic<bool> s_busy{false};
static std::string s_desc, s_status;                 // what is loaded; the last upload's result
static size_t s_size = 0;
static FILE* s_up = nullptr;                         // upload in progress
static size_t s_upDone = 0, s_upTotal = 0;
static int64_t s_upUs = 0;

// Reads a map file into PSRAM and checks it. Returns the buffer (caller owns) or nullptr.
static uint8_t* loadFile(const char* path, size_t& len, std::string& desc, std::string& why) {
    struct stat st;
    if (stat(path, &st) != 0) { why = "no file"; return nullptr; }
    len = (size_t)st.st_size;
    if (len < 16 || len > kMax) { why = "wrong size"; return nullptr; }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { why = "not enough memory"; return nullptr; }
    FILE* f = fopen(path, "rb");
    size_t got = f ? fileRead(f, (char*)buf, len) : 0;
    if (f) fclose(f);
    if (got != len) { free(buf); why = "read failed"; return nullptr; }
    long n = mapdata::check(buf, len, &desc);
    if (n <= 0) { free(buf); why = "damaged"; return nullptr; }
    if (desc.empty()) desc = "map file";
    ESP_LOGI(TAG, "%s: %ld polylines, %u bytes: %s", path, n, (unsigned)len, desc.c_str());
    return buf;
}

static void use(uint8_t* buf, size_t len, const std::string& desc) {
    s_desc = desc;
    s_size = len;
#if CONFIG_IC7100_DISPLAY
    ui_set_map(buf, len);                             // the touch screen takes the buffer over
#else
    free(buf);
#endif
}

static void loadTask(void*) {
    size_t len = 0;
    std::string desc, why;
    if (uint8_t* buf = loadFile(kPath, len, desc, why)) {
        use(buf, len, desc);
        if (s_app && s_app->logFn) s_app->logFn(("APRS map: detailed map loaded (" + desc + ")").c_str());
    } else if (why != "no file") {
        ESP_LOGW(TAG, "%s not used: %s", kPath, why.c_str());
        if (s_app && s_app->logFn) s_app->logFn(("APRS map: map.rdm on the card not used (" + why + ")").c_str());
    }
    vTaskDelete(nullptr);
}

void mapfile_start(App& app, bool haveCard) {
    s_app = &app;
    if (haveCard) xTaskCreate(loadTask, "mapload", 4096, nullptr, 2, nullptr);
}

static esp_err_t json(httpd_req_t* req, const std::string& js) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, js.data(), js.size());
}
static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 32) o += c; }
    return o;
}
static esp_err_t reply(httpd_req_t* req, bool ok, const std::string& msg, bool retry = false) {
    s_status = msg;
    ESP_LOGI(TAG, "%s", msg.c_str());
    return json(req, std::string("{\"ok\":") + (ok ? "true" : "false") + (retry ? ",\"retry\":true" : "") +
                         (ok ? ",\"msg\":\"" : ",\"err\":\"") + esc(msg) + "\"}");
}
static bool queryNum(httpd_req_t* req, const char* key, uint32_t& out) {
    char q[128], v[24];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return false;
    out = (uint32_t)strtoul(v, nullptr, 0);
    return true;
}
static bool recvAll(httpd_req_t* req, uint8_t* p, size_t n) {
    int timeouts = 0;
    while (n) {
        int r = httpd_req_recv(req, (char*)p, n);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) { if (++timeouts > 5) return false; continue; }
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}
static void upAbort() {
    if (s_up) { fclose(s_up); s_up = nullptr; remove(kTmp); }
    s_upDone = s_upTotal = 0;
}

// GET /api/map: what is loaded. POST /api/map?del=1: remove the detailed map (back to the built-in outlines).
static esp_err_t hInfo(httpd_req_t* req) {
    if (req->method == HTTP_POST) {
        uint32_t del = 0;
        if (queryNum(req, "del", del) && del) {
            if (s_busy.exchange(true)) return reply(req, false, "A map upload is running");
            remove(kPath);
            s_desc.clear();
            s_size = 0;
#if CONFIG_IC7100_DISPLAY
            ui_set_map(nullptr, 0);
#endif
            s_busy = false;
            return reply(req, true, "Detailed map removed; the built-in outlines are used");
        }
        return reply(req, false, "bad request");
    }
    struct stat st;
    bool card = stat(SDCARD_LOG_DIR, &st) == 0;
    return json(req, std::string("{\"card\":") + (card ? "true" : "false") + ",\"have\":" + (s_size ? "true" : "false") +
                         ",\"size\":" + std::to_string(s_size) + ",\"desc\":\"" + esc(s_desc) + "\",\"status\":\"" + esc(s_status) + "\"}");
}

// POST /api/mapchunk?off=N&total=T&crc=C, pieces of up to 64 kB in order (CRC-32 each; a damaged one is asked
// again). The last piece checks the whole file before it replaces map.rdm.
static esp_err_t hChunk(httpd_req_t* req) {
    uint32_t off = 0, total = 0, crc = 0;
    if (!queryNum(req, "off", off) || !queryNum(req, "total", total) || !queryNum(req, "crc", crc))
        return reply(req, false, "bad request");
    size_t n = req->content_len;
    if (total < 16 || total > kMax) return reply(req, false, "That is not a RigDeck map file (wrong size)");
    if (n == 0 || n > kPiece || off + n > total) return reply(req, false, "bad piece size");
    struct stat st;
    if (stat(SDCARD_LOG_DIR, &st) != 0) return reply(req, false, "No microSD card: the detailed map is kept on the card");
    if (s_busy.exchange(true)) return reply(req, false, "A map upload is already running");
    struct Done { ~Done() { s_busy = false; } } done;
    if (s_up && esp_timer_get_time() - s_upUs > 60000000LL) upAbort();       // stale upload
    static uint8_t* piece = nullptr;
    if (!piece) piece = (uint8_t*)heap_caps_malloc(kPiece, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!piece) return reply(req, false, "Not enough memory");
    if (!recvAll(req, piece, n)) return reply(req, false, "piece interrupted", true);
    if (esp_rom_crc32_le(0, piece, n) != crc) return reply(req, false, "damaged piece", true);
    if (off == 0) {
        upAbort();
        if (memcmp(piece, "RDM2", 4) != 0 && memcmp(piece, "RDM1", 4) != 0)
            return reply(req, false, "That is not a RigDeck map file");
        s_up = fopen(kTmp, "wb");
        if (!s_up) return reply(req, false, "Could not write to the microSD card");
        s_upTotal = total;
    } else if (!s_up || total != s_upTotal || off != s_upDone) {
        if (s_up && off < s_upDone && off + n <= s_upDone) return reply(req, true, "have");   // a repeat of a piece we have
        return reply(req, false, "Pieces out of order. Start again.");
    }
    if (!fileWrite(s_up, (const char*)piece, n)) { upAbort(); return reply(req, false, "Writing to the microSD card failed (card full?)"); }
    s_upDone += n;
    s_upUs = esp_timer_get_time();
    if (s_upDone < s_upTotal) return reply(req, true, "Saving " + std::to_string(s_upDone * 100 / s_upTotal) + " %");
    fclose(s_up);
    s_up = nullptr;
    size_t len = 0;
    std::string desc, why;
    uint8_t* buf = loadFile(kTmp, len, desc, why);
    if (!buf) { remove(kTmp); s_upDone = s_upTotal = 0; return reply(req, false, "The map file arrived " + why + "; nothing was changed"); }
    remove(kPath);                                    // FAT rename does not replace a file
    bool renamed = rename(kTmp, kPath) == 0;
    s_upDone = s_upTotal = 0;
    use(buf, len, desc);
    if (s_app && s_app->logFn) s_app->logFn(("APRS map: detailed map installed (" + desc + ")").c_str());
    return reply(req, renamed, renamed ? "Detailed map installed: " + desc : "Map loaded, but it could not be saved on the card");
}

void mapfile_register(httpd_handle_t srv) {
    static const httpd_uri_t a = {"/api/map", HTTP_GET, hInfo, nullptr};
    static const httpd_uri_t b = {"/api/map", HTTP_POST, hInfo, nullptr};
    static const httpd_uri_t c = {"/api/mapchunk", HTTP_POST, hChunk, nullptr};
    httpd_register_uri_handler(srv, &a);
    httpd_register_uri_handler(srv, &b);
    httpd_register_uri_handler(srv, &c);
}
