// Starts the LCD, touch panel and LVGL through Waveshare's board support package, then builds the
// operating screen. Kept in C and close to Waveshare's own LVGL example for this board.
#include "sdkconfig.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui.h"

static const char *TAG = "display";

void display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    lv_display_t *display = bsp_display_start_with_config(&cfg);
    if (display == NULL) {
        ESP_LOGE(TAG, "display did not start; continuing with the web interface only");
        return;
    }
    bsp_display_backlight_on();
    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "could not take the LVGL lock");
        return;
    }
    ui_create();
    bsp_display_unlock();
    ESP_LOGI(TAG, "touch interface started (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
}
