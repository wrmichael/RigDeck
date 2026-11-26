// K0DOG, the QSL-card dog game of the CW trainer. The game is not in the firmware (flash is scarce): RigDeck
// downloads it from the RigDeck site to the microSD card (RigDeck/k0dog/) when it is on a home network, keeps it
// up to date there, and serves it from the card. No card, or not downloaded yet: the K0DOG button stays hidden
// (App::k0dogReady). The album of QSL cards that came back lives on the card too (RigDeck/k0dog_album.jsonl).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "adif.h"
#include "mapfile.h"
#include "sdcard.h"

static const char* TAG = "k0dog";
static const char* kSite = "https://wrmichael.github.io/RigDeck/k0dog/";
static const char* kDir = SDCARD_LOG_DIR "/k0dog";
static const size_t kMaxShow = 200, kMaxCard = 1500, kMaxFile = 2u << 20;
static App* s_app = nullptr;
static std::mutex s_mtx;                              // album file

struct GameFile { const char* name; const char* type; bool gz; };
static const GameFile kFiles[] = {{"k0dog.html.gz", "text/html; charset=utf-8", true}, {"k0dog.jpg", "image/jpeg", false}};

static bool haveCard() { struct stat st; return stat(SDCARD_LOG_DIR, &st) == 0; }
static std::string path(const char* name) { return std::string(kDir) + "/" + name; }
static long fileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }

static std::string readSmall(const std::string& p) {
    std::string s;
    if (FILE* f = fopen(p.c_str(), "r")) {
        char b[256];
        size_t n = fread(b, 1, sizeof b - 1, f);
        s.assign(b, n);
        fclose(f);
    }
    return s;
}
static std::string jsonStr(const std::string& js, const char* key) {
    size_t a = js.find(std::string("\"") + key + "\"");
    if (a == std::string::npos) return "";
    size_t q1 = js.find('"', js.find(':', a) + 1), q2 = q1 == std::string::npos ? q1 : js.find('"', q1 + 1);
    return q2 == std::string::npos ? "" : js.substr(q1 + 1, q2 - q1 - 1);
}
static long jsonNum(const std::string& js, const std::string& key) {
    size_t a = js.find("\"" + key + "\"");
    return a == std::string::npos ? -1 : strtol(js.c_str() + js.find(':', a) + 1, nullptr, 10);
}

// Every game file there, with the size the stored description says.
static bool installed() {
    std::string js = readSmall(path("k0dog.json"));
    if (js.empty()) return false;
    for (const auto& f : kFiles) {
        long want = jsonNum(js, f.name);
        if (want <= 0 || fileSize(path(f.name)) != want) return false;
    }
    return true;
}

static bool download(const std::string& name, size_t want, std::string& err) {
    std::string tmp = path(name.c_str()) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = "cannot write " + tmp; return false; }
    size_t got = 0;
    err = web_get(std::string(kSite) + name, [&](const char* p, int n, int64_t) {
        if (got + n > kMaxFile || !fileWrite(f, p, (size_t)n)) return false;
        got += (size_t)n;
        return true;
    });
    fclose(f);
    if (err.empty() && got != want) err = name + ": " + std::to_string(got) + " of " + std::to_string(want) + " bytes";
    if (!err.empty()) { remove(tmp.c_str()); return false; }
    remove(path(name.c_str()).c_str());
    return rename(tmp.c_str(), path(name.c_str()).c_str()) == 0;
}

// Checks the RigDeck site for the game (first after start-up, then once a day) and fetches a new version.
static void syncTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(120000));                // after start-up, the update check and the first decodes
    for (;;) {
        int waitMin = 30;
        if (haveCard()) {
            mkdir(kDir, 0777);
            s_app->k0dogReady = installed();
            if (s_app->staStatus().state == "connected") {
                std::string js, err;
                err = web_get(std::string(kSite) + "k0dog.json", [&](const char* p, int n, int64_t) { js.append(p, n); return js.size() < 2048; });
                std::string ver = jsonStr(js, "version");
                if (err.empty() && !ver.empty()) {
                    bool ok = true;
                    if (ver != jsonStr(readSmall(path("k0dog.json")), "version") || !installed()) {
                        ESP_LOGI(TAG, "getting K0DOG %s from the RigDeck site", ver.c_str());
                        for (const auto& f : kFiles) {
                            long want = jsonNum(js, f.name);
                            if (want <= 0 || (size_t)want > kMaxFile || !download(f.name, (size_t)want, err)) { ok = false; break; }
                        }
                        if (ok) {
                            if (FILE* f = fopen(path("k0dog.json").c_str(), "w")) { fputs(js.c_str(), f); fclose(f); }
                            if (s_app->logFn) s_app->logFn(("K0DOG game " + ver + " saved on the microSD card").c_str());
                        } else ESP_LOGW(TAG, "K0DOG download failed: %s", err.c_str());
                    }
                    s_app->k0dogReady = installed();
                    waitMin = ok ? 24 * 60 : 30;
                } else ESP_LOGW(TAG, "RigDeck site not reached: %s", err.c_str());
            }
        } else s_app->k0dogReady = false;
        vTaskDelay(pdMS_TO_TICKS(waitMin * 60000));
    }
}

void k0dog_start(App& app) {
    s_app = &app;
    if (haveCard()) app.k0dogReady = installed();
    xTaskCreate(syncTask, "k0dog", 10240, nullptr, 2, nullptr);
}

// Sends one game file from the card in pieces (the page is stored gzip-compressed).
static esp_err_t sendFile(httpd_req_t* req, const GameFile& g) {
    FILE* f = s_app && s_app->k0dogReady ? fopen(path(g.name).c_str(), "rb") : nullptr;
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "K0DOG is not on the microSD card yet");
    httpd_resp_set_type(req, g.type);
    if (g.gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char* buf = (char*)malloc(4096);
    esp_err_t r = buf ? ESP_OK : ESP_FAIL;
    size_t n;
    while (r == ESP_OK && (n = fileRead(f, buf, 4096)) > 0) r = httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    free(buf);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, nullptr, 0);
    return r;
}
static esp_err_t hPage(httpd_req_t* req) { return sendFile(req, kFiles[0]); }
static esp_err_t hJpg(httpd_req_t* req) { return sendFile(req, kFiles[1]); }

static std::string albumPath() { return haveCard() ? std::string(SDCARD_LOG_DIR) + "/k0dog_album.jsonl" : ""; }

static esp_err_t hAlbumGet(httpd_req_t* req) {
    std::deque<std::string> cards;
    std::string p = albumPath();
    if (!p.empty()) {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (FILE* f = fopen(p.c_str(), "r")) {
            static char line[kMaxCard + 4];
            while (fgets(line, sizeof line, f)) {
                size_t n = strlen(line);
                while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
                if (n < 2 || line[0] != '{') continue;
                cards.emplace_back(line, n);
                if (cards.size() > kMaxShow) cards.pop_front();
            }
            fclose(f);
        }
    }
    std::string js = std::string("{\"sd\":") + (p.empty() ? "false" : "true") + ",\"cards\":[";
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
    std::string p = albumPath();
    bool ok = !p.empty();
    if (ok) {
        std::lock_guard<std::mutex> lk(s_mtx);
        FILE* f = fopen(p.c_str(), "a");
        ok = f && fputs(body.c_str(), f) >= 0 && fputc('\n', f) != EOF;
        if (f) fclose(f);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void k0dog_register(httpd_handle_t srv) {
    static const httpd_uri_t u[] = {{"/k0dog", HTTP_GET, hPage, nullptr}, {"/k0dog.jpg", HTTP_GET, hJpg, nullptr},
                                    {"/api/album", HTTP_GET, hAlbumGet, nullptr}, {"/api/album", HTTP_POST, hAlbumPost, nullptr}};
    for (const auto& x : u) httpd_register_uri_handler(srv, &x);
}
