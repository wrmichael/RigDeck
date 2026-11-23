// LVGL's memory in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC). With the C library malloc, LVGL's many small
// drawing allocations (40 x ~480 bytes in a burst) came from internal RAM, which on the ESP32-P4 is
// the same scarce DMA-capable RAM that USB, the SD card, Wi-Fi and the SHA engine need. Nothing LVGL
// allocates here is used by DMA directly (the display frame buffers are allocated by the BSP).
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include <stdlib.h>
#include "esp_heap_caps.h"

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) { (void)mem; (void)bytes; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void* lv_malloc_core(size_t size) {
    void* p = heap_caps_malloc(size, LV_CAPS);
    return p ? p : malloc(size);
}
void* lv_realloc_core(void* p, size_t new_size) {
    void* q = heap_caps_realloc(p, new_size, LV_CAPS);
    return q ? q : realloc(p, new_size);
}
void lv_free_core(void* p) { heap_caps_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) { (void)mon_p; }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif
