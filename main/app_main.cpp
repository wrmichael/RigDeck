#include <string>
#include <atomic>
// RigDeck - ESP-IDF entry point.
#include "platform.h"
#include "ui.h"
#include "clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include <sys/time.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include "sdcard.h"
#include "sdlog.h"
#include "mapfile.h"
#endif

static const char* TAG = "main";
static App s_app;

static int64_t monoMs() { return esp_timer_get_time() / 1000; }

// The chip keeps its own clock (system time) across restarts, and across power-off when the board's
// backup cell holds the RTC domain up. Every sync is copied into it; at boot it is read back.
static void rtcSink(int64_t utcMs) {
    struct timeval tv = {(time_t)(utcMs / 1000), (suseconds_t)((utcMs % 1000) * 1000)};
    settimeofday(&tv, nullptr);
}
static void rtcRestore() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    if (ms > 1767225600000LL) {                       // after 2026-01-01: it was set before, not counting from zero
        clk::setUtcMs(ms, "rtc");
        ESP_LOGI(TAG, "clock restored from the chip's clock (%lld s since 1970); it is re-checked when GPS, NTP or a browser is available",
                 (long long)tv.tv_sec);
    }
}
static bool haveRam(size_t bytes) {
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT) >= bytes &&
           heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) >= 200 * 1024;
}

// Size and use of the internal store or the microSD card, for the Device page and "log almost full".
static bool fsInfo(bool sd, uint64_t& total, uint64_t& used) {
    if (!sd) {
        size_t t = 0, u = 0;
        if (esp_spiffs_info("storage", &t, &u) != ESP_OK) return false;
        total = t; used = u;
        return true;
    }
#if CONFIG_IDF_TARGET_ESP32P4
    return sdcard_info(&total, &used);
#else
    return false;
#endif
}

static void decodeTask(void*) { s_app.decodeLoop(); }
static void netlogTask(void*) { s_app.netLoop(); }

#if CONFIG_IDF_TARGET_ESP32P4
extern std::atomic<const char*> g_webNow;
extern std::atomic<bool> g_sdlogBusy;
#endif

// Internal-RAM low-point tracer: each time the lowest-ever free internal RAM drops by 1 kB or more
// (after the first minute), one line says what was going on at that moment, so the peak can be
// traced to its cause (TX starting, a decode, a web request, a card write, Wi-Fi...).
extern "C" void memtrace_recent(char* out, size_t cap, int ms);
// LVGL's memory functions live in main/lv_mem_psram.c; LVGL is linked after main, so pull that
// object in explicitly (the linker would otherwise not take it from libmain.a).
extern "C" void* lv_malloc_core(size_t size);
void* volatile g_keepLvMem = (void*)&lv_malloc_core;

// A failed allocation is noted here (any task) and reported by the control task.
static std::atomic<uint32_t> s_failSize{0}, s_failCaps{0};
static std::atomic<TaskHandle_t> s_failTask{nullptr};
static void allocFailed(size_t size, uint32_t caps, const char* fn) {
    (void)fn;
    s_failSize = (uint32_t)size; s_failCaps = caps; s_failTask = xTaskGetCurrentTaskHandle();
}

static void lowPointTrace(int64_t now) {
    if (uint32_t fs = s_failSize.exchange(0)) {
        TaskHandle_t t = s_failTask.load();
        ESP_LOGW(TAG, "memory: an allocation of %u bytes (caps 0x%x) failed in task %s",
                 (unsigned)fs, (unsigned)s_failCaps.load(), t ? pcTaskGetName(t) : "?");
    }
    static size_t reported = 0;
    // Internal DMA-capable RAM is the scarce part (USB, SD card, Wi-Fi link, SHA engine need it).
    size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (now < 60000) { reported = low; return; }
    if (reported && low + 1024 > reported) return;
    reported = low;
    std::string what;
    auto add = [&](const char* w) { what += what.empty() ? "" : ", "; what += w; };
    if (s_app.txOnNow()) add("transmitting");
    if (s_app.decodingNow()) add("decoding");
#if CONFIG_IDF_TARGET_ESP32P4
    if (const char* w = g_webNow.load()) { add("web request: "); what += w; }
    if (g_sdlogBusy) add("console log card write");
#endif
    if (what.empty()) add("nothing RigDeck tracks (Wi-Fi/USB background)");
    char who[240];
    memtrace_recent(who, sizeof who, 150);
    if (who[0]) { what += "; big internal allocations in the last 150 ms: "; what += who; }
    ESP_LOGW(TAG, "RAM low point: internal DMA-capable %u kB lowest (now %u kB free, largest block %u kB) while %s",
             (unsigned)(low / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024), what.c_str());
}

static void controlTask(void*) {
    heap_caps_register_failed_alloc_callback(allocFailed);
    int64_t lastMem = 0;
    for (;;) {
        s_app.tick();
        lowPointTrace(monoMs());
#if CONFIG_IDF_TARGET_ESP32P4
        sdlog_enable(s_app.consoleLogOn());
#endif
        int64_t now = monoMs();
        if (now - lastMem >= 60000) {                  // memory once a minute: a leak shows as a falling "internal" figure
            lastMem = now;
            ESP_LOGI(TAG, "memory: internal %u kB free (lowest %u kB, largest block %u kB; DMA-capable %u kB, lowest %u kB), PSRAM %u kB free",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            App::StaStatus w = s_app.staStatus();            // and the home network, to line up with "can't reach it"
            if (w.state != "off")
                ESP_LOGI(TAG, "home network: %s %s %s%s%s", w.state.c_str(), w.ssid.c_str(), w.ip.c_str(),
                         w.reason.empty() ? "" : ", ", w.reason.c_str());
        }
        if (s_app.rebootReq) {
            for (int i = 0; i < 60; i++) { s_app.tick(); vTaskDelay(pdMS_TO_TICKS(10)); }   // let PTT-off go out
            usbradio_stop_for_restart();
            vTaskDelay(pdMS_TO_TICKS(700));                  // the console log task writes the last lines to the card
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void) {
#if CONFIG_IDF_TARGET_ESP32P4
    sdlog_capture();                                  // console log text is kept from here on (for the card)
#endif
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_vfs_spiffs_conf_t fs = {};
    fs.base_path = "/fs";
    fs.partition_label = "storage";
    fs.max_files = 6;
    fs.format_if_mount_failed = true;
    err = esp_vfs_spiffs_register(&fs);
    if (err != ESP_OK) ESP_LOGE(TAG, "SPIFFS mount failed (%s): settings and log will not persist", esp_err_to_name(err));

    clk::init(monoMs);
    rtcRestore();
    clk::setSink(rtcSink);
    s_app.haveRam = haveRam;
    s_app.fwVersion = esp_app_get_description()->version;
    s_app.logFn = [](const char* line) { ESP_LOGI("app", "%s", line); };
    std::string logDir;                               // the log goes on the microSD card when one is in
#if CONFIG_IDF_TARGET_ESP32P4
    char sdi[64];
    if (sdcard_start(sdi, sizeof sdi)) {
        logDir = SDCARD_LOG_DIR;
        sdlog_start(SDCARD_LOG_DIR "/logs");          // console log onto the card too
    } else sdlog_off();
    s_app.sdInfo = sdi;
#endif
    s_app.fsInfo = fsInfo;
#if CONFIG_IDF_TARGET_ESP32P4
    s_app.sdFormat = [](std::string& msg) {
        char m[96];
        int r = sdcard_format(m, sizeof m);
        msg = m;
        return r;
    };
#endif
    ota_start(s_app);
    s_app.begin("/fs", logDir);
    ESP_LOGI(TAG, "RigDeck %s, call=%s grid=%s, free heap %u kB", s_app.fwVersion.c_str(), s_app.cfg.call.c_str(), s_app.cfg.grid.c_str(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));

    net_start(s_app);
    web_start(s_app);
#if CONFIG_IDF_TARGET_ESP32P4
    wifichip_start(s_app);
    mapfile_start(s_app, !logDir.empty());
#endif
    usbradio_start(s_app);
    gps_module_start(s_app);
#if CONFIG_IC7100_DISPLAY
    ui_bind(&s_app);
    display_start();
#endif
    xTaskCreatePinnedToCore(decodeTask, "ft8dec", 24576, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(netlogTask, "netlog", 6144, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 8, nullptr, 0);
}
