// K0DOG, the QSL-card dog game of the CW trainer: the page (gzip, built by tools/build_k0dog.py) and its
// album of QSL cards that came back. The album lives on the microSD card (RigDeck/k0dog_album.jsonl, one card
// per line); without a card the last kRam cards are kept in memory until the next restart.
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "sdcard.h"
#endif

extern const uint8_t k0dog_gz_start[] asm("_binary_k0dog_html_gz_start");
extern const uint8_t k0dog_gz_end[] asm("_binary_k0dog_html_gz_end");

static const char* TAG = "k0dog";
static const size_t kRam = 24, kMaxShow = 200, kMaxCard = 1500;
static std::mutex s_mtx;
static std::deque<std::string> s_ram;

static std::string albumPath() {
#if CONFIG_IDF_TARGET_ESP32P4
    struct stat st;
    if (stat(SDCARD_LOG_DIR, &st) == 0) return std::string(SDCARD_LOG_DIR) + "/k0dog_album.jsonl";
#endif
    return "";
}

static esp_err_t hPage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char*)k0dog_gz_start, k0dog_gz_end - k0dog_gz_start);
}

static esp_err_t hAlbumGet(httpd_req_t* req) {
    std::deque<std::string> cards;
    std::string path = albumPath();
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!path.empty()) {
            if (FILE* f = fopen(path.c_str(), "r")) {
                char line[kMaxCard + 4];
                while (fgets(line, sizeof line, f)) {
                    size_t n = strlen(line);
                    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
                    if (n < 2 || line[0] != '{') continue;
                    cards.emplace_back(line, n);
                    if (cards.size() > kMaxShow) cards.pop_front();
                }
                fclose(f);
            }
        } else cards = s_ram;
    }
    std::string js = std::string("{\"sd\":") + (path.empty() ? "false" : "true") + ",\"max\":" + std::to_string(kRam) + ",\"cards\":[";
    for (size_t i = 0; i < cards.size(); i++) js += (i ? "," : "") + cards[i];
    js += "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, js.data(), js.size());
}

static esp_err_t hAlbumPost(httpd_req_t* req) {
    size_t n = req->content_len;
    if (n < 2 || n > kMaxCard) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad card");
    std::string body(n, '\0');
    size_t got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, &body[got], n - got);
        if (r <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cut off");
        got += (size_t)r;
    }
    for (char& c : body) if (c == '\n' || c == '\r') c = ' ';   // one card per line
    if (body[0] != '{' || body.back() != '}') return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a card");
    std::string path = albumPath();
    bool ok = true;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (!path.empty()) {
            FILE* f = fopen(path.c_str(), "a");
            ok = f && fputs(body.c_str(), f) >= 0 && fputc('\n', f) != EOF;
            if (f) fclose(f);
        } else {
            s_ram.push_back(body);
            while (s_ram.size() > kRam) s_ram.pop_front();
        }
    }
    if (!ok) ESP_LOGW(TAG, "album: could not write %s", path.c_str());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void k0dog_register(httpd_handle_t srv) {
    static const httpd_uri_t a = {"/k0dog", HTTP_GET, hPage, nullptr};
    static const httpd_uri_t b = {"/api/album", HTTP_GET, hAlbumGet, nullptr};
    static const httpd_uri_t c = {"/api/album", HTTP_POST, hAlbumPost, nullptr};
    httpd_register_uri_handler(srv, &a);
    httpd_register_uri_handler(srv, &b);
    httpd_register_uri_handler(srv, &c);
}
