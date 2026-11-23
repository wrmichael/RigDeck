// Who takes internal RAM at its low points (used by the low-point tracer in app_main.cpp).
// ESP-IDF calls esp_heap_trace_alloc_hook() after every allocation (CONFIG_HEAP_USE_HOOKS). Each
// internal-RAM allocation of 256 bytes or more goes into a small ring: time, size, task. When the
// tracer sees a new low point it sums up the last 100 ms of the ring per task.
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_HEAP_USE_HOOKS
#define RING 64
typedef struct { int64_t us; uint32_t size; TaskHandle_t task; } Rec;
static DRAM_ATTR Rec s_ring[RING];
static DRAM_ATTR volatile uint32_t s_next;

void IRAM_ATTR esp_heap_trace_alloc_hook(void* ptr, size_t size, uint32_t caps) {
    (void)caps;
    if (!ptr || size < 256 || !esp_ptr_internal(ptr)) return;
    uint32_t i = __atomic_fetch_add(&s_next, 1, __ATOMIC_RELAXED) % RING;
    s_ring[i].us = esp_timer_get_time();
    s_ring[i].size = (uint32_t)size;
    s_ring[i].task = xTaskGetCurrentTaskHandle();
}

// "tiT 6x1600=9 kB, httpd 2x4096=8 kB" for the last `ms` milliseconds; empty if nothing.
void memtrace_recent(char* out, size_t cap, int ms) {
    struct { TaskHandle_t t; uint32_t n, sum, max; } agg[8];
    int na = 0;
    int64_t from = esp_timer_get_time() - (int64_t)ms * 1000;
    for (int i = 0; i < RING; i++) {
        Rec r = s_ring[i];
        if (!r.size || r.us < from) continue;
        int k = 0;
        while (k < na && agg[k].t != r.task) k++;
        if (k == na) { if (na == 8) continue; agg[na].t = r.task; agg[na].n = agg[na].sum = agg[na].max = 0; na++; }
        agg[k].n++; agg[k].sum += r.size; if (r.size > agg[k].max) agg[k].max = r.size;
    }
    out[0] = 0;
    size_t len = 0;
    for (int k = 0; k < na && len + 48 < cap; k++) {
        const char* nm = agg[k].t ? pcTaskGetName(agg[k].t) : "?";
        char name[17];
        strncpy(name, nm ? nm : "?", 16);
        name[16] = 0;
        len += snprintf(out + len, cap - len, "%s%s %ux (max %u B) = %u B", len ? ", " : "", name,
                        (unsigned)agg[k].n, (unsigned)agg[k].max, (unsigned)agg[k].sum);
    }
}
#else
void memtrace_recent(char* out, size_t cap, int ms) { (void)ms; if (cap) out[0] = 0; }
#endif
