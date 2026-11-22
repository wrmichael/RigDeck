// microSD card for the log (ESP32-P4 board only).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SDCARD_LOG_DIR "/sdcard/RigDeck"
bool sdcard_start(char *info, size_t n);           // true = mounted; info: what the card is, or why not
bool sdcard_info(uint64_t *total, uint64_t *used);
int sdcard_format(char *msg, size_t n);            // 0 = formatted (msg: what it is now), -1 = failed (msg: why)
#ifdef __cplusplus
}
#endif
