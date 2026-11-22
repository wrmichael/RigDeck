// Console log copied to files on the microSD card (see sdlog.cpp).
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void sdlog_capture(void);            // first thing at start-up: keep log text in memory
void sdlog_start(const char* dir);   // once the card is mounted: write it to dir, as it happens
void sdlog_off(void);
void sdlog_enable(bool on);          // the Settings switch (on by default)                // no card: stop keeping text
void sdlog_pause(bool pause);
void sdlog_register(void* httpServer); // web page: /api/consolelogs (list), /consolelog?f=name        // true: close the file and wait (card about to be formatted)
#ifdef __cplusplus
}
#endif
