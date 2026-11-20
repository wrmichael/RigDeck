// Containers kept in PSRAM. Everything below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4 kB) normally
// comes from the P4's small internal RAM, which the USB host (CAT, audio), the Wi-Fi link and the
// display need. Log records, worked-before and "sent" keys are many small strings, so they use
// this allocator instead (plain malloc on the host).
#pragma once
#include <cstdlib>
#include <deque>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

template <class T> struct PsAlloc {
    using value_type = T;
    PsAlloc() noexcept {}
    template <class U> PsAlloc(const PsAlloc<U>&) noexcept {}
    T* allocate(size_t n) {
#ifdef ESP_PLATFORM
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = malloc(n * sizeof(T));
#else
        void* p = malloc(n * sizeof(T));
#endif
        if (!p) std::abort();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) noexcept { free(p); }
    template <class U> bool operator==(const PsAlloc<U>&) const noexcept { return true; }
    template <class U> bool operator!=(const PsAlloc<U>&) const noexcept { return false; }
};

using PStr = std::basic_string<char, std::char_traits<char>, PsAlloc<char>>;
inline PStr pstr(std::string_view s) { return PStr(s.data(), s.size()); }
inline std::string sstr(std::string_view s) { return std::string(s.data(), s.size()); }
struct SvLess {                                    // compare PStr / std::string / string_view alike
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const { return a < b; }
};
using PSet = std::set<PStr, SvLess, PsAlloc<PStr>>;
using RecVec = std::vector<PStr, PsAlloc<PStr>>;  // one ADIF record per entry
