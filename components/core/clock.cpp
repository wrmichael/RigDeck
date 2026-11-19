#include "clock.h"
#include <atomic>
namespace clk {
static MonoFn g_mono = nullptr;
static std::atomic<int64_t> g_offset{0};
static std::atomic<int64_t> g_syncMono{0};
static std::atomic<bool> g_valid{false};
static std::atomic<const char*> g_src{"none"};
static SinkFn g_sink = nullptr;
void init(MonoFn fn) { g_mono = fn; }
void setSink(SinkFn fn) { g_sink = fn; }
int64_t monoMs() { return g_mono ? g_mono() : 0; }
int64_t utcMs() { return monoMs() + g_offset.load(); }
bool valid() { return g_valid.load(); }
void setUtcMs(int64_t utc, const char* source) {
    g_offset.store(utc - monoMs());
    g_syncMono.store(monoMs());
    g_src.store(source);
    g_valid.store(true);
    if (g_sink) g_sink(utc);
}
void trim(int ms) { g_offset.fetch_add(ms); if (g_sink) g_sink(utcMs()); }
const char* source() { return g_src.load(); }
int64_t ageMs() { return monoMs() - g_syncMono.load(); }
}
