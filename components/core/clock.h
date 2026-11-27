// UTC timekeeping. FT8/FT4 need the clock right to within a few hundred ms, and the board has no
// battery-backed RTC, so UTC = (monotonic ms since boot) + an offset set by NTP or by the browser.
#pragma once
#include <cstdint>
namespace clk {
typedef int64_t (*MonoFn)();
void init(MonoFn fn);              // platform supplies a monotonic millisecond counter
typedef void (*SinkFn)(int64_t utcMs);
void setSink(SinkFn fn);           // called after every set/trim (the ESP32 copies it into its RTC clock)
int64_t monoMs();
int64_t utcMs();                   // 0 offset (i.e. garbage) until valid()
bool valid();
void setUtcMs(int64_t utc, const char* source);
void trim(int ms);                 // manual nudge
const char* source();              // "none", "browser", "ntp", "gps", "gps-pps", "gps-nmea", "gps-usb", "rtc"
int64_t ageMs();                   // ms since last sync
}
