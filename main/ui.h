// On-device touch interface. ui_bind() first, then ui_create() with the LVGL lock held.
#pragma once
#ifdef __cplusplus
class App;
void ui_bind(App* app);
#include <cstddef>
#include <cstdint>
// Detailed APRS map (mapfile.cpp): the screen takes over the malloc'd buffer; nullptr = built-in outlines again.
// Any thread; the swap happens on the LVGL thread.
void ui_set_map(uint8_t* data, size_t len);
extern "C" {
#endif
void ui_create(void);
extern void (*ui_brightness_fn)(int percent);   // set by the display code: backlight, %
#ifdef __cplusplus
}
#endif
