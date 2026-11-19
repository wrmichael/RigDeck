// GPS module on the board's header: NMEA on a UART, and optionally its PPS output on a GPIO.
// Wiring (Settings > Clock): module TX -> gpsRxPin, PPS -> gpsPpsPin, VCC -> 3.3 V or 5 V (as the
// module needs), GND -> GND. The module's RX is not needed.
#include "app.h"
#include "clock.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char* TAG = "gps";
static App* s_app = nullptr;
static const uart_port_t kUart = UART_NUM_1;          // UART0 is the console

static void IRAM_ATTR onPps(void*) {
    s_app->ppsMono32.store((uint32_t)(esp_timer_get_time() / 1000));
    s_app->ppsCount.fetch_add(1);
}

static void gpsTask(void*) {
    static uint8_t buf[256];
    uint32_t lastPps = 0;
    int64_t lastReport = 0;
    for (;;) {
        int n = uart_read_bytes(kUart, buf, sizeof buf, pdMS_TO_TICKS(20));
        if (n > 0) s_app->gpsModuleRx(buf, (size_t)n);
        int64_t now = esp_timer_get_time() / 1000;
        if (now - lastReport > 60000) {                   // once a minute: is the module alive?
            uint32_t p = s_app->ppsCount.load();
            ESP_LOGI(TAG, "module: %s, %u PPS pulses in the last minute, clock source %s",
                     now - s_app->gpsLastMono.load() < 10000 ? "time received" : "no time yet (no fix?)",
                     (unsigned)(p - lastPps), clk::source());
            lastPps = p;
            lastReport = now;
        }
    }
}

static bool pinOk(int pin) {
    // 0/1: 32 kHz crystal, 7/8: touch I2C, 37/38: console, 14-19/54: Wi-Fi co-processor link
    static const int reserved[] = {0, 1, 7, 8, 14, 15, 16, 17, 18, 19, 37, 38, 54};
    if (pin < 0 || pin > 54) return false;
    for (int r : reserved) if (pin == r) return false;
    return true;
}

void gps_module_start(App& app) {
    s_app = &app;
    if (!app.cfg.gpsModule) return;
    const int rx = app.cfg.gpsRxPin, pps = app.cfg.gpsPpsPin;
    if (!pinOk(rx) || (pps >= 0 && (!pinOk(pps) || pps == rx))) {
        ESP_LOGE(TAG, "GPS module pins RX=%d PPS=%d not usable on this board; module disabled", rx, pps);
        return;
    }
    uart_config_t c = {};
    c.baud_rate = app.cfg.gpsModBaud;
    c.data_bits = UART_DATA_8_BITS;
    c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1;
    c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(kUart, 1024, 0, 0, nullptr, 0) != ESP_OK || uart_param_config(kUart, &c) != ESP_OK ||
        uart_set_pin(kUart, UART_PIN_NO_CHANGE, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "GPS module UART setup failed");
        return;
    }
    if (pps >= 0) {
        gpio_config_t g = {};
        g.pin_bit_mask = 1ULL << pps;
        g.mode = GPIO_MODE_INPUT;
        g.pull_down_en = GPIO_PULLDOWN_ENABLE;
        g.intr_type = GPIO_INTR_POSEDGE;
        gpio_config(&g);
        esp_err_t e = gpio_install_isr_service(0);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "GPIO interrupt service: %s", esp_err_to_name(e));
        gpio_isr_handler_add((gpio_num_t)pps, onPps, nullptr);
    }
    xTaskCreate(gpsTask, "gps", 4096, nullptr, 6, nullptr);
    ESP_LOGI(TAG, "GPS module: RX on GPIO%d at %d baud, PPS %s%d", rx, app.cfg.gpsModBaud, pps >= 0 ? "on GPIO" : "not used ", pps);
}
