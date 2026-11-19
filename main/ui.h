// On-device touch interface. ui_bind() first, then ui_create() with the LVGL lock held.
#pragma once
#ifdef __cplusplus
class App;
void ui_bind(App* app);
extern "C" {
#endif
void ui_create(void);
#ifdef __cplusplus
}
#endif
