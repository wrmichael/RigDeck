// IC-7100 FT8/FT4/PSK31 controller - ESP-IDF entry point.
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
#include "nvs_flash.h"

static const char* TAG = "main";
static App s_app;

static int64_t monoMs() { return esp_timer_get_time() / 1000; }
static bool haveRam(size_t bytes) {
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT) >= bytes &&
           heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) >= 200 * 1024;
}

static void decodeTask(void*) { s_app.decodeLoop(); }
static void netlogTask(void*) { s_app.netLoop(); }

static void controlTask(void*) {
    for (;;) {
        s_app.tick();
        if (s_app.rebootReq) {
            for (int i = 0; i < 60; i++) { s_app.tick(); vTaskDelay(pdMS_TO_TICKS(10)); }   // let PTT-off go out
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void) {
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
    s_app.haveRam = haveRam;
    s_app.begin("/fs");
    ESP_LOGI(TAG, "call=%s grid=%s, free heap %u kB", s_app.cfg.call.c_str(), s_app.cfg.grid.c_str(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));

    net_start(s_app);
    web_start(s_app);
    usbradio_start(s_app);
#if CONFIG_IC7100_DISPLAY
    ui_bind(&s_app);
    display_start();
#endif
    xTaskCreatePinnedToCore(decodeTask, "ft8dec", 24576, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(netlogTask, "netlog", 6144, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 8, nullptr, 0);
}
