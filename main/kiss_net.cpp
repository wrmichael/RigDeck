// KISS TNC over TCP (default port 8001): PC programs (Winlink Express, Pat, YAAC, APRSIS32, Linux AX.25 ...)
// use RigDeck's packet modem over the network. Frames from a program go out only while TX is enabled
// (App::pktTick); frames heard go to every program connected. Works on the hotspot and the home network.
#include "kiss_net.h"
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "ax25.h"

static const char* TAG = "kiss";
static App* s_app = nullptr;

struct Client { int fd = -1; ax25::KissDecoder dec; };

static void task(void*) {
    int lis = -1, port = 0;
    Client cl[2];
    uint8_t buf[512];
    for (;;) {
        int want = s_app->cfg.kissPort;
        if (want != port) {                                   // started, or the port changed in Settings
            if (lis >= 0) { close(lis); lis = -1; }
            for (auto& c : cl) if (c.fd >= 0) { close(c.fd); c.fd = -1; }
            port = want;
            if (port > 0) {
                lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                int on = 1;
                setsockopt(lis, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
                sockaddr_in a = {};
                a.sin_family = AF_INET;
                a.sin_port = htons((uint16_t)port);
                a.sin_addr.s_addr = htonl(INADDR_ANY);
                if (lis < 0 || bind(lis, (sockaddr*)&a, sizeof a) != 0 || listen(lis, 2) != 0) {
                    ESP_LOGW(TAG, "port %d not available", port);
                    if (lis >= 0) close(lis);
                    lis = -1;
                } else ESP_LOGI(TAG, "KISS TNC on TCP port %d", port);
            }
        }
        if (lis < 0) { s_app->kissClients = 0; vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lis, &rd);
        int mx = lis;
        for (auto& c : cl) if (c.fd >= 0) { FD_SET(c.fd, &rd); if (c.fd > mx) mx = c.fd; }
        timeval tv = {0, 50000};
        int r = select(mx + 1, &rd, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(lis, &rd)) {
            int fd = accept(lis, nullptr, nullptr);
            if (fd >= 0) {
                Client* slot = nullptr;
                for (auto& c : cl) if (c.fd < 0) slot = &c;
                if (!slot) close(fd);
                else {
                    int on = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
                    slot->fd = fd;
                    slot->dec = ax25::KissDecoder();
                    if (s_app->logFn) s_app->logFn("KISS: a program connected");
                }
            }
        }
        for (auto& c : cl) {
            if (c.fd < 0 || r <= 0 || !FD_ISSET(c.fd, &rd)) continue;
            int n = recv(c.fd, buf, sizeof buf, 0);
            if (n <= 0) {
                close(c.fd);
                c.fd = -1;
                if (s_app->logFn) s_app->logFn("KISS: a program disconnected");
                continue;
            }
            std::vector<std::vector<uint8_t>> frames;
            c.dec.feed(buf, (size_t)n, frames);
            for (auto& f : frames)
                if (!f.empty() && (f[0] & 0x0F) == 0)                 // data frame on any port; TXDELAY etc. ignored
                    s_app->kissFromHost(std::vector<uint8_t>(f.begin() + 1, f.end()));
        }
        int nc = 0;
        for (auto& c : cl) if (c.fd >= 0) nc++;
        s_app->kissClients = nc;
        if (nc) {
            std::vector<std::vector<uint8_t>> heard;
            s_app->kissTake(heard);
            for (auto& h : heard) {
                std::vector<uint8_t> k = ax25::kissWrap(h);
                for (auto& c : cl)
                    if (c.fd >= 0 && send(c.fd, k.data(), k.size(), 0) < 0) { close(c.fd); c.fd = -1; }
            }
        }
    }
}

void kiss_start(App& app) {
    s_app = &app;
    xTaskCreate(task, "kiss", 4096, nullptr, 3, nullptr);
}
