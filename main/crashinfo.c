// Crash report kept across the restart: what ESP_ERROR_CHECK, abort() or a CPU exception said is written into
// RAM that a software restart does not clear, and app_main logs it on the next start (so it reaches the console
// log on the microSD card, not only the USB console). Hooked in with the linker's --wrap (main/CMakeLists.txt).
#include <stdint.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_private/panic_internal.h"
#include "crashinfo.h"

#define CRASH_MAGIC 0x52444b43u
typedef struct {
    uint32_t magic;
    char text[400];
} crash_t;
static __NOINIT_ATTR crash_t s_crash;

static IRAM_ATTR void put(const char* a) {
    if (!a) return;
    size_t n = strnlen(s_crash.text, sizeof s_crash.text - 1);
    while (*a && n < sizeof s_crash.text - 1) s_crash.text[n++] = *a++;
    s_crash.text[n] = 0;
}
static IRAM_ATTR void putHex(uint32_t v) {
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) { int d = (v >> (28 - 4 * i)) & 15; b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
    b[10] = 0;
    put(b);
}
static IRAM_ATTR void putDec(int v) {
    char b[12];
    int i = 11;
    b[i] = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    if (v < 0) b[--i] = '-';
    put(b + i);
}
static IRAM_ATTR void start(void) {
    if (s_crash.magic != CRASH_MAGIC) { s_crash.magic = CRASH_MAGIC; s_crash.text[0] = 0; }
    else put(" | ");
}

void __real__esp_error_check_failed(esp_err_t rc, const char* file, int line, const char* function, const char* expression) __attribute__((__noreturn__));
void __wrap__esp_error_check_failed(esp_err_t rc, const char* file, int line, const char* function, const char* expression) {
    start();
    put("ESP_ERROR_CHECK failed: ");
    put(esp_err_to_name(rc));
    put(" in ");
    put(function);
    put("() at ");
    const char* f = file ? strrchr(file, '/') : NULL;
    put(f ? f + 1 : file);
    put(":");
    putDec(line);
    put(": ");
    put(expression);
    __real__esp_error_check_failed(rc, file, line, function, expression);
}

void __real_esp_panic_handler(panic_info_t* info);
IRAM_ATTR void __wrap_esp_panic_handler(panic_info_t* info) {
    start();
    if (g_panic_abort_details) put(g_panic_abort_details);
    else {
        put(info->reason ? info->reason : "panic");
        if (info->description) { put(" ("); put(info->description); put(")"); }
        put(" at PC ");
        putHex((uint32_t)(uintptr_t)info->addr);
        put(" on core ");
        putDec(info->core);
    }
    __real_esp_panic_handler(info);
}

bool crashinfo_take(char* out, size_t len) {
    if (s_crash.magic != CRASH_MAGIC) return false;
    s_crash.magic = 0;
    s_crash.text[sizeof s_crash.text - 1] = 0;
    strlcpy(out, s_crash.text, len);
    return out[0] != 0;
}
