// On-device touch interface. ui_bind() first, then ui_create() with the LVGL lock held.
#pragma once
#ifdef __cplusplus
class App;
void ui_bind(App* app);
extern "C" {
#endif
void ui_create(void);
extern void (*ui_brightness_fn)(int percent);   // set by the display code: backlight, %
#ifdef __cplusplus
}
#endif
