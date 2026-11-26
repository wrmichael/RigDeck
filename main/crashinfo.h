#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// The last crash's report (ESP_ERROR_CHECK, abort or exception), once, after a crash restart; false if none.
bool crashinfo_take(char* out, size_t len);
#ifdef __cplusplus
}
#endif
