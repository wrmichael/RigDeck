// HTTP server: the control page plus a small JSON API the page polls.
#include "platform.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char* TAG = "web";
static App* s_app;
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static std::string urlDecode(const char* s, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char h[3] = {s[i + 1], s[i + 2], 0};
            o += (char)strtol(h, nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

static void parseForm(const std::string& body, std::map<std::string, std::string>& out) {
    size_t p = 0;
    while (p < body.size()) {
        size_t amp = body.find('&', p);
        if (amp == std::string::npos) amp = body.size();
        size_t eq = body.find('=', p);
        if (eq != std::string::npos && eq < amp)
            out[urlDecode(body.data() + p, eq - p)] = urlDecode(body.data() + eq + 1, amp - eq - 1);
        p = amp + 1;
    }
}

static std::string query(httpd_req_t* req) {
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0 || len > 1024) return "";
    std::string q(len + 1, 0);
    if (httpd_req_get_url_query_str(req, &q[0], len + 1) != ESP_OK) return "";
    q.resize(len);
    return q;
}

static esp_err_t sendJson(httpd_req_t* req, const std::string& js) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, js.data(), js.size());
}

static esp_err_t hIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t n = index_html_end - index_html_start;
    if (n && index_html_start[n - 1] == 0) n--;      // EMBED_TXTFILES appends a NUL
    return httpd_resp_send(req, index_html_start, n);
}

static esp_err_t hState(httpd_req_t* req) {
    std::map<std::string, std::string> a;
    parseForm(query(req), a);
    return sendJson(req, s_app->stateJson((uint32_t)strtoul(a["since"].c_str(), nullptr, 10),
                                           (uint32_t)strtoul(a["psk"].c_str(), nullptr, 10), a["cfg"] == "1"));
}

static esp_err_t hSpec(httpd_req_t* req) { return sendJson(req, s_app->spectrumJson()); }

static esp_err_t hCmd(httpd_req_t* req) {
    if (req->content_len > 2048) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
    std::string body(req->content_len, 0);
    size_t got = 0;
    while (got < body.size()) {
        int r = httpd_req_recv(req, &body[got], body.size() - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    std::map<std::string, std::string> a;
    parseForm(body, a);
    std::string c = a["c"];
    a.erase("c");
    return sendJson(req, s_app->command(c, a));
}

static esp_err_t hLog(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");   // the page supplies a dated file name
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    bool archive = req->user_ctx != nullptr;
    FILE* f = fopen((archive ? s_app->archivePath() : s_app->logPath()).c_str(), "r");
    if (!f) {                                        // nothing logged yet: still a valid, empty ADIF file
        std::string h = adifHeader();
        return httpd_resp_send(req, h.data(), h.size());
    }
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { fclose(f); return ESP_FAIL; }
    fclose(f);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

void web_start(App& app) {
    s_app = &app;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "HTTP server failed to start"); return; }
    static const httpd_uri_t uris[] = {
        {"/", HTTP_GET, hIndex, nullptr},
        {"/api/state", HTTP_GET, hState, nullptr},
        {"/api/spec", HTTP_GET, hSpec, nullptr},
        {"/api/cmd", HTTP_POST, hCmd, nullptr},
        {"/log.adi", HTTP_GET, hLog, nullptr},
        {"/archive.adi", HTTP_GET, hLog, (void*)1},
    };
    for (auto& u : uris) httpd_register_uri_handler(srv, &u);
}
