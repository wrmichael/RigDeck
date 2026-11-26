// RigDecks find each other on the home network (core/peers.h): a UDP broadcast line every 30 s on port 47731,
// and a list of the stations heard for the all-stations page (/api/peers). Only on the home network (station
// interface): the hotspot is not announced.
#include "peers_net.h"
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "clock.h"
#include "peers.h"

static const char* TAG = "peers";
static App* s_app = nullptr;
static std::mutex s_mtx;
static std::vector<peers::Peer> s_list;

static void sendLine(int sock, const std::string& line) {
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(peers::kPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(sock, line.data(), line.size(), 0, (sockaddr*)&to, sizeof to);
}

static void task(void*) {
    int sock = -1;
    int64_t lastSend = 0;
    std::string lastIp;
    char buf[160];
    for (;;) {
        App::StaStatus sta = s_app->staStatus();
        bool up = sta.state == "connected" && !sta.ip.empty();
        if (!up) {
            if (sock >= 0) { close(sock); sock = -1; }
            lastIp.clear();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
            int on = 1;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
            timeval tv = {1, 0};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            sockaddr_in me = {};
            me.sin_family = AF_INET;
            me.sin_port = htons(peers::kPort);
            me.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(sock, (sockaddr*)&me, sizeof me) != 0) {
                ESP_LOGW(TAG, "port %u busy", (unsigned)peers::kPort);
                close(sock); sock = -1;
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
        }
        int64_t now = clk::monoMs();
        auto me = [&] { return peers::announce(sta.ip, s_app->cfg.call, s_app->fwVersion, s_app->cfg.rigName); };
        if (sta.ip != lastIp) {                        // just joined: ask everyone, and say who we are
            lastIp = sta.ip;
            sendLine(sock, "RIGDECK1 ?");
            sendLine(sock, me());
            lastSend = now;
        } else if (now - lastSend >= 30000) {
            sendLine(sock, me());
            lastSend = now;
        }
        sockaddr_in from = {};
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
        if (n > 0) {
            buf[n] = 0;
            std::string line(buf);
            peers::Peer p;
            if (peers::isQuery(line)) {
                vTaskDelay(pdMS_TO_TICKS(50 + (esp_random() % 400)));   // not all at once
                sendLine(sock, me());
            } else if (peers::parse(line, p) && p.ip != sta.ip) {
                char src[16];
                inet_ntoa_r(from.sin_addr, src, sizeof src);
                if (p.ip == src) {                        // the sender's own address only
                    std::lock_guard<std::mutex> lk(s_mtx);
                    bool isNew = true;
                    for (auto& q : s_list) if (q.ip == p.ip) isNew = false;
                    peers::heard(s_list, p, clk::monoMs());
                    if (isNew) ESP_LOGI(TAG, "found RigDeck at %s (%s)", p.ip.c_str(), p.name.empty() ? p.call.c_str() : p.name.c_str());
                }
            }
        }
        std::lock_guard<std::mutex> lk(s_mtx);
        peers::expire(s_list, clk::monoMs(), 120000);
    }
}

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 32) o += c; }
    return o;
}

// GET /api/peers: this RigDeck and the ones it heard in the last 2 minutes.
static esp_err_t hPeers(httpd_req_t* req) {
    App::StaStatus sta = s_app->staStatus();
    std::string js = "{\"me\":{\"ip\":\"" + esc(sta.state == "connected" ? sta.ip : "") + "\",\"name\":\"" + esc(s_app->cfg.rigName) +
                     "\",\"call\":\"" + esc(s_app->cfg.call) + "\",\"ver\":\"" + esc(s_app->fwVersion) + "\"},\"peers\":[";
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        int64_t now = clk::monoMs();
        bool first = true;
        for (const auto& p : s_list) {
            js += std::string(first ? "" : ",") + "{\"ip\":\"" + esc(p.ip) + "\",\"name\":\"" + esc(p.name) + "\",\"call\":\"" + esc(p.call) +
                  "\",\"ver\":\"" + esc(p.ver) + "\",\"age\":" + std::to_string((now - p.seenMono) / 1000) + "}";
            first = false;
        }
    }
    js += "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");   // the page may be a file on a computer
    return httpd_resp_send(req, js.data(), js.size());
}

void peers_start(App& app) {
    s_app = &app;
    xTaskCreate(task, "peers", 4096, nullptr, 2, nullptr);
}

void peers_register(httpd_handle_t srv) {
    static const httpd_uri_t u = {"/api/peers", HTTP_GET, hPeers, nullptr};
    httpd_register_uri_handler(srv, &u);
}
