// Console log to the microSD card: everything the board prints (ESP_LOG) also goes into
// /sdcard/RigDeck/logs/console_YYYYMMDD_HHMM.txt. A new file is started at each start-up, when the
// clock gets set, and at UTC midnight. Without a clock the name is console_19761018_0000.txt (one
// file, appended to; started fresh once it passes 5 MB, never removed by age). Files dated more
// than 7 days back are removed. Log calls only copy their text into a buffer in PSRAM; a task writes it to the card, so
// logging never waits for the card and the card driver's own messages cannot loop.
#include "sdlog.h"
#include "clock.h"
#include "adif.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"

static const size_t RING = 64 * 1024;
static char* s_ring;
static size_t s_head, s_tail;                  // write / read positions (mod RING)
static unsigned s_dropped;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;
static std::string s_dir;
static std::atomic<bool> s_off{false};          // no card: stop keeping text
static std::atomic<bool> s_enabled{true};       // the Settings switch
static std::atomic<bool> s_pause{false}, s_closed{true};   // card being formatted: file closed

static int teeVprintf(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
    char line[256];
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (n > 0 && s_ring && !s_off && s_enabled) {
        if (n > (int)sizeof line - 1) n = sizeof line - 1;
        portENTER_CRITICAL_SAFE(&s_mux);
        size_t used = s_head - s_tail;
        if (used + n <= RING) {
            for (int i = 0; i < n; i++) s_ring[(s_head + i) % RING] = line[i];
            s_head += n;
        } else s_dropped++;
        portEXIT_CRITICAL_SAFE(&s_mux);
    }
    return r;
}

void sdlog_capture(void) {
    if (s_ring) return;
    s_ring = (char*)heap_caps_malloc(RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) return;
    s_prev = esp_log_set_vprintf(teeVprintf);
}

static std::string stamp(int64_t ms, bool withTime) {    // YYYYMMDD_HHMM, or the no-clock date
    if (!clk::valid()) return withTime ? "19761018_0000" : "19761018";
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char b[20];
    strftime(b, sizeof b, withTime ? "%Y%m%d_%H%M" : "%Y%m%d", &tm);
    return b;
}

static void purgeOld() {                       // console files dated more than 7 days back
    if (!clk::valid()) return;
    std::string cutoff = stamp(clk::utcMs() - 7LL * 86400000LL, false);
    std::vector<std::string> gone;
    if (DIR* d = opendir(s_dir.c_str())) {
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.compare(0, 8, "console_") == 0 && n.size() >= 16 && n.substr(8, 8) < cutoff && n.substr(8, 8) != "19761018")
                gone.push_back(n);
        }
        closedir(d);
    }
    for (auto& n : gone) remove((s_dir + "/" + n).c_str());
}

// Buffers in PSRAM: internal RAM is kept for the USB host (CAT, audio), the Wi-Fi link and the display.
static const size_t CHUNK = 4096;
static char* s_chunk;

std::atomic<bool> g_sdlogBusy{false};                  // for the internal-RAM low-point tracer
static void writerTask(void*) {
    char* chunk = s_chunk;
    FILE* f = nullptr;
    std::string day;
    bool clockAtOpen = false;
    int64_t lastMark = 0;
    int sinceSync = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_pause || !s_enabled) {                           // card being formatted, or switched off
            if (f) { fclose(f); f = nullptr; }
            s_closed = true;
            continue;
        }
        s_closed = false;
        bool valid = clk::valid();
        int64_t now = valid ? clk::utcMs() : 0;
        std::string today = stamp(now, false);
        if (!f || valid != clockAtOpen || today != day) {      // new file: start-up, clock set, new day
            if (f) fclose(f);
            std::string path = s_dir + "/console_" + stamp(now, true) + ".txt";
            struct stat st;
            if (!valid && stat(path.c_str(), &st) == 0 && st.st_size > 5 * 1024 * 1024) remove(path.c_str());
            f = fopen(path.c_str(), "a");
            day = today;
            clockAtOpen = valid;
            lastMark = 0;
            purgeOld();
            if (f) fprintf(f, "==== RigDeck console log, %s ====\n", valid ? (stamp(now, true) + " UTC").c_str() : "clock not set");
        }
        if (!f) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
        if (valid && now - lastMark >= 60000) {                // a wall-clock mark once a minute
            lastMark = now;
            time_t t = (time_t)(now / 1000);
            struct tm tm;
            gmtime_r(&t, &tm);
            char b[48];
            strftime(b, sizeof b, "---- %Y-%m-%d %H:%M:%S UTC ----\n", &tm);
            fputs(b, f);
        }
        for (;;) {
            size_t n = 0;
            unsigned dropped;
            portENTER_CRITICAL(&s_mux);
            while (s_tail != s_head && n < CHUNK) chunk[n++] = s_ring[s_tail++ % RING];
            dropped = s_dropped;
            s_dropped = 0;
            portEXIT_CRITICAL(&s_mux);
            if (dropped) fprintf(f, "[console log: %u messages lost, buffer full]\n", dropped);
            if (!n) break;
            g_sdlogBusy = true;
            fileWrite(f, chunk, n);                                // via internal RAM (see adif.h)
            g_sdlogBusy = false;
        }
        fflush(f);
        if (++sinceSync >= 10) { sinceSync = 0; fsync(fileno(f)); }   // on the card every ~5 s
    }
}

void sdlog_off(void) { s_off = true; }
void sdlog_enable(bool on) { s_enabled = on; }

void sdlog_pause(bool pause) {
    s_pause = pause;
    if (pause) mkdir(s_dir.c_str(), 0777);
    for (int i = 0; pause && !s_closed && i < 40; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!pause) mkdir(s_dir.c_str(), 0777);                    // the folder is gone after a format
}

void sdlog_start(const char* dir) {
    if (!s_ring) return;
    s_chunk = (char*)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_chunk) return;
    s_dir = dir;
    mkdir(s_dir.c_str(), 0777);
    xTaskCreate(writerTask, "sdlog", 3584, nullptr, 2, nullptr);
}

// ---- web page: list and download the console logs ----------------------------------------------
static bool goodName(const std::string& n) {
    return n.compare(0, 8, "console_") == 0 && n.size() < 64 && n.find('/') == std::string::npos &&
           n.find("..") == std::string::npos && n.size() > 12 && n.compare(n.size() - 4, 4, ".txt") == 0;
}

static esp_err_t hList(httpd_req_t* req) {
    std::vector<std::pair<std::string, long>> files;
    if (!s_dir.empty())
        if (DIR* d = opendir(s_dir.c_str())) {
            while (struct dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (!goodName(n)) continue;
                struct stat st;
                long sz = stat((s_dir + "/" + n).c_str(), &st) == 0 ? (long)st.st_size : 0;
                files.emplace_back(n, sz);
            }
            closedir(d);
        }
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });   // newest first
    std::string o = std::string("{\"ok\":true,\"on\":") + (s_dir.empty() || s_off ? "false" : "true") + ",\"files\":[";
    for (size_t i = 0; i < files.size(); i++)
        o += (i ? "," : "") + std::string("{\"name\":\"") + files[i].first + "\",\"size\":" + std::to_string(files[i].second) + "}";
    o += "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, o.data(), o.size());
}

static esp_err_t hFile(httpd_req_t* req) {
    char q[96] = "", f[72] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "f", f, sizeof f) != ESP_OK || !goodName(f)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad file name");
        return ESP_FAIL;
    }
    FILE* fp = fopen((s_dir + "/" + f).c_str(), "r");
    if (!fp) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such log"); return ESP_FAIL; }
    httpd_resp_set_type(req, "text/plain");
    std::string disp = std::string("attachment; filename=\"") + f + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    alignas(64) char buf[1024];                 // internal RAM: the card's DMA reads into it (see adif.h)
    size_t n;
    esp_err_t r = ESP_OK;
    while (r == ESP_OK && (n = fread(buf, 1, sizeof buf, fp)) > 0) r = httpd_resp_send_chunk(req, buf, n);
    fclose(fp);
    if (r != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

void sdlog_register(void* srv) {
    static const httpd_uri_t l = {"/api/consolelogs", HTTP_GET, hList, nullptr};
    static const httpd_uri_t g = {"/consolelog", HTTP_GET, hFile, nullptr};
    httpd_register_uri_handler((httpd_handle_t)srv, &l);
    httpd_register_uri_handler((httpd_handle_t)srv, &g);
}
