// APRS map on the microSD card (RigDeck/map.rdm, built by tools/make_map.py and published on the GitHub site).
// It gets there three ways: RigDeck downloads it itself (home network with internet), the web page downloads it
// and sends it here in pieces, or the user copies the file onto the card. It is checked whole, then kept in PSRAM
// for the touch screen. Without it the APRS screen shows rings and stations only.
#include "sdkconfig.h"                              // CONFIG_IC7100_DISPLAY: hand the map to the touch screen
#include "mapfile.h"
#include "platform.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>
#include <cctype>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include <mutex>
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
    ESP_LOGI(TAG, "handing %u bytes to the touch screen", (unsigned)len);
    ui_set_map(buf, len);                             // the touch screen takes the buffer over
#else
    ESP_LOGW(TAG, "built without the touch screen: map not used");
    free(buf);
#endif
}

static void loadTask(void*) {
    size_t len = 0;
    std::string desc, why;
    // Copied onto the card by hand under the published name: take it as map.rdm.
    static const char* kCopied = SDCARD_LOG_DIR "/rigdeck-map.rdm";
    struct stat st;
    if (stat(kPath, &st) != 0 && stat(kCopied, &st) == 0 && rename(kCopied, kPath) == 0)
        if (s_app && s_app->logFn) s_app->logFn("APRS map: rigdeck-map.rdm on the card renamed to map.rdm");
    if (uint8_t* buf = loadFile(kPath, len, desc, why)) {
        use(buf, len, desc);
        if (s_app && s_app->logFn) s_app->logFn(("APRS map: map loaded (" + desc + ")").c_str());
    } else if (why != "no file") {
        ESP_LOGW(TAG, "%s not used: %s", kPath, why.c_str());
        if (s_app && s_app->logFn) s_app->logFn(("APRS map: map.rdm on the card not used (" + why + ")").c_str());
    }
    vTaskDelete(nullptr);
}

// ---- the board fetches the map itself (touch screen button), when it is on a home network with internet ----
static const char* kSite = "https://wrmichael.github.io/RigDeck/map/";
static std::mutex s_fMtx;
static std::string s_fMsg;                           // shown on the touch screen
static int64_t s_fMsgUs = 0;
static std::atomic<bool> s_fetching{false};

static void fmsg(const std::string& m) {
    std::lock_guard<std::mutex> lk(s_fMtx);
    if (m != s_fMsg) ESP_LOGI(TAG, "%s", m.c_str());
    s_fMsg = m;
    s_fMsgUs = esp_timer_get_time();
}

// GET url; each piece of the body goes to sink (false = stop). Returns "" or why it failed.
std::string web_get(const std::string& url, const std::function<bool(const char*, int, int64_t)>& sink) {
    esp_http_client_config_t c = {};
    c.url = url.c_str();
    c.timeout_ms = 15000;
    c.buffer_size = 4096;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    c.crt_bundle_attach = esp_crt_bundle_attach;
#else
    return "this build has no certificate bundle for HTTPS";
#endif
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return "could not start the download";
    std::string err;
    if (esp_http_client_open(h, 0) != ESP_OK) err = "could not reach the RigDeck site (no internet?)";
    else {
        int64_t len = esp_http_client_fetch_headers(h);
        int st = esp_http_client_get_status_code(h);
        if (st != 200) err = "the RigDeck site answered " + std::to_string(st);
        else {
            char* buf = (char*)malloc(4096);
            if (!buf) err = "not enough memory";
            while (buf) {
                int n = esp_http_client_read(h, buf, 4096);
                if (n < 0) { err = "the download was interrupted"; break; }
                if (n == 0) { if (!esp_http_client_is_complete_data_received(h)) err = "the download was cut off"; break; }
                if (!sink(buf, n, len)) { err = "stopped"; break; }
            }
            free(buf);
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return err;
}

static void fetchRun() {
    std::string js;
    std::string e = web_get(std::string(kSite) + "map.json", [&](const char* p, int n, int64_t) {
        js.append(p, n);
        return js.size() < 4096;
    });
    std::string file;
    size_t want = 0;
    if (e.empty()) {
        size_t a = js.find("\"file\"");
        size_t q1 = a == std::string::npos ? a : js.find('"', js.find(':', a) + 1);
        size_t q2 = q1 == std::string::npos ? q1 : js.find('"', q1 + 1);
        if (q2 != std::string::npos) file = js.substr(q1 + 1, q2 - q1 - 1);
        size_t b = js.find("\"size\"");
        if (b != std::string::npos) want = (size_t)strtoul(js.c_str() + js.find(':', b) + 1, nullptr, 10);
        bool ok = !file.empty() && want >= 16 && want <= kMax;
        for (char ch : file) if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') ok = false;
        if (!ok) e = "the RigDeck site has no map description";
    }
    if (!e.empty()) { fmsg("Map: " + e); return; }
    while (s_busy.exchange(true)) vTaskDelay(pdMS_TO_TICKS(200));    // not during a page upload
    FILE* f = fopen(kTmp, "wb");
    if (!f) { s_busy = false; fmsg("Map: could not write to the microSD card"); return; }
    size_t got = 0;
    int lastPct = -1;
    e = web_get(std::string(kSite) + file, [&](const char* p, int n, int64_t) {
        if (got + n > want || !fileWrite(f, p, (size_t)n)) return false;
        got += (size_t)n;
        int pct = (int)(got * 100 / want);
        if (pct != lastPct) { lastPct = pct; fmsg("Map: downloading " + std::to_string(pct) + " %"); }
        return true;
    });
    fclose(f);
    if (e.empty() && got != want) e = "the download is incomplete (" + std::to_string(got) + " of " + std::to_string(want) + " bytes)";
    if (!e.empty()) { remove(kTmp); s_busy = false; fmsg("Map: " + e + "; nothing was changed"); return; }
    size_t len = 0;
    std::string desc, why;
    uint8_t* buf = loadFile(kTmp, len, desc, why);
    if (!buf) { remove(kTmp); s_busy = false; fmsg("Map: the file arrived " + why + "; nothing was changed"); return; }
    remove(kPath);
    bool renamed = rename(kTmp, kPath) == 0;
    s_busy = false;
    use(buf, len, desc);
    s_status = renamed ? "Detailed map installed: " + desc : "Map loaded, but it could not be saved on the card";
    if (s_app && s_app->logFn) s_app->logFn(("APRS map: map installed by the board (" + desc + ")").c_str());
    fmsg(renamed ? "Map: installed" : "Map: loaded, but it could not be saved on the card");
}

static void fetchTask(void*) {
    fetchRun();
    s_fetching = false;
    vTaskDelete(nullptr);                             // does not return: nothing after it runs
}

static bool fetchStart() {
    struct stat st;
    if (stat(SDCARD_LOG_DIR, &st) != 0) { fmsg("Map: a microSD card is needed for the map"); return false; }
    if (!s_app || s_app->staStatus().state != "connected") {
        fmsg("Map: RigDeck needs a home network with internet for this (Settings, Wi-Fi)");
        return false;
    }
    if (s_fetching.exchange(true)) return false;
    fmsg("Map: contacting the RigDeck site ...");
    if (xTaskCreate(fetchTask, "mapfetch", 10240, nullptr, 3, nullptr) != pdPASS) {
        s_fetching = false;
        fmsg("Map: not enough memory to start the download");
        return false;
    }
    return true;
}

static std::string fetchStatus() {
    std::lock_guard<std::mutex> lk(s_fMtx);
    if (!s_fetching && esp_timer_get_time() - s_fMsgUs > 30000000LL) return "";   // a result stays for 30 s
    return s_fMsg;
}

void mapfile_start(App& app, bool haveCard) {
    s_app = &app;
#if CONFIG_IC7100_DISPLAY
    ui_map_fetch_fn = fetchStart;
    ui_map_status_fn = fetchStatus;
#endif
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

// GET /api/map: what is loaded. POST /api/map?del=1: remove the map.
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
            return reply(req, true, "Map removed from the microSD card");
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
    if (stat(SDCARD_LOG_DIR, &st) != 0) return reply(req, false, "No microSD card: the map is kept on the card");
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
    if (s_app && s_app->logFn) s_app->logFn(("APRS map: map installed (" + desc + ")").c_str());
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
