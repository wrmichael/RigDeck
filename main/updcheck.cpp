// The board looks at the RigDeck site every 6 hours while it is on a home network, so the touch screen and
// the web page can say "update available" (the decision is ver::offer in core/version.cpp).
#include "mapfile.h"
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char* TAG = "updcheck";
static App* s_app = nullptr;

static std::string versionAt(const std::string& url, std::string& err) {
    std::string js;
    err = web_get(url, [&](const char* p, int n, int64_t) { js.append(p, n); return js.size() < 4096; });
    if (!err.empty()) return "";
    size_t a = js.find("\"version\"");
    if (a == std::string::npos) { err = "no version in " + url; return ""; }
    size_t q1 = js.find('"', js.find(':', a) + 1), q2 = q1 == std::string::npos ? q1 : js.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 - q1 > 24) { err = "bad version in " + url; return ""; }
    return js.substr(q1 + 1, q2 - q1 - 1);
}

static void task(void*) {
    vTaskDelay(pdMS_TO_TICKS(90000));                 // after start-up, NTP and the first decodes
    for (;;) {
        int waitMin = 10;
        if (s_app->staStatus().state == "connected") {
            std::string e1, e2;
            // Each board has its own folder on the site (RIGDECK_SITE_DIR: "" for the 7B, "nano/" for the NANO).
            std::string st = versionAt(std::string("https://wrmichael.github.io/RigDeck/") + RIGDECK_SITE_DIR + "stable/manifest.json", e1);
            std::string be = versionAt(std::string("https://wrmichael.github.io/RigDeck/") + RIGDECK_SITE_DIR + "manifest.json", e2);
            if (!st.empty() || !be.empty()) {
                s_app->setReleases(st, be);
                ESP_LOGI(TAG, "RigDeck site: Stable %s, Beta %s", st.empty() ? "?" : st.c_str(), be.empty() ? "?" : be.c_str());
                waitMin = 6 * 60;
            } else ESP_LOGW(TAG, "RigDeck site not reached: %s", e1.c_str());
        }
        // One minute at a time: pdMS_TO_TICKS(6 h) overflows FreeRTOS's 32-bit tick arithmetic (ms x 1000 Hz) and
        // came out as about 2 minutes, so the site was asked every 2 minutes instead of every 6 hours.
        for (int m = 0; m < waitMin; m++) vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void updcheck_start(App& app) {
    s_app = &app;
    xTaskCreate(task, "updcheck", 10240, nullptr, 2, nullptr);
}
