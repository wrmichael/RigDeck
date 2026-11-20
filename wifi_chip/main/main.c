// RigDeck Wi-Fi chip firmware: ESP-Hosted co-processor (Wi-Fi over SDIO to the ESP32-P4).
// Everything is done by the ESP-Hosted component; this only prepares storage and the event loop.
// Based on Espressif's ESP-Hosted example (Apache-2.0).
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

void app_main(void)
{
    ESP_LOGI("rigdeck_wifi", "RigDeck Wi-Fi chip firmware starting");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}
