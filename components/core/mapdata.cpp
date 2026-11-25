#include "mapdata.h"
#include <cstring>
#include <vector>

namespace mapdata {

static int16_t rd16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint32_t rdu32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static int32_t rd32(const uint8_t* p) { return (int32_t)rdu32(p); }

// Decodes n-1 steps from d[p..end) into la/lo (already holding the first point), times mul.
static bool decode(const uint8_t* d, size_t& p, size_t end, int n, int32_t a, int32_t o, int mul, int32_t* la, int32_t* lo) {
    if (la) { la[0] = a * mul; lo[0] = o * mul; }
    for (int i = 1; i < n; i++) {
        if (p >= end) return false;
        int da, dd;
        if ((int8_t)d[p] == -128) {
            if (p + 5 > end) return false;
            da = rd16(d + p + 1); dd = rd16(d + p + 3); p += 5;
        } else {
            if (p + 2 > end) return false;
            da = (int8_t)d[p]; dd = (int8_t)d[p + 1]; p += 2;
        }
        a += da; o += dd;
        if (la) { la[i] = a * mul; lo[i] = o * mul; }
    }
    return true;
}

static long walk(const uint8_t* d, size_t len, int lat0, int lon0, int lat1, int lon1, unsigned mask,
                 const std::function<void(int, const int32_t*, const int32_t*, int)>* fn, std::string* desc) {
    if (!d || len < 8) return -1;
    bool v2 = memcmp(d, "RDM2", 4) == 0;
    if (!v2 && memcmp(d, "RDM1", 4) != 0) return -1;
    uint32_t count = rdu32(d + 4);
    size_t p = 8;
    if (v2) {
        if (p + 2 > len) return -1;
        size_t dn = d[p] | (d[p + 1] << 8);
        if (p + 2 + dn > len) return -1;
        if (desc) desc->assign((const char*)d + p + 2, dn);
        p += 2 + dn;
    } else if (desc) desc->clear();
    std::vector<int32_t> la, lo;
    for (uint32_t k = 0; k < count; k++) {
        int cls, n;
        int32_t b0, b1, b2, b3, a, o;
        size_t end = len;
        int mul;
        if (v2) {
            if (p + 31 > len) return -1;
            cls = d[p]; n = d[p + 1] | (d[p + 2] << 8);
            uint32_t sb = rdu32(d + p + 3);
            b0 = rd32(d + p + 7); b1 = rd32(d + p + 11); b2 = rd32(d + p + 15); b3 = rd32(d + p + 19);
            a = rd32(d + p + 23); o = rd32(d + p + 27);
            p += 31;
            if (sb > len - p) return -1;
            end = p + sb;
            mul = 1;
        } else {
            if (p + 15 > len) return -1;
            cls = d[p]; n = d[p + 1] | (d[p + 2] << 8);
            b0 = rd16(d + p + 3) * 10; b1 = rd16(d + p + 5) * 10; b2 = rd16(d + p + 7) * 10; b3 = rd16(d + p + 9) * 10;
            a = rd16(d + p + 11); o = rd16(d + p + 13);
            p += 15;
            mul = 10;
        }
        bool want = fn && (mask >> (cls & 31) & 1) && !(b2 < lat0 || b0 > lat1 || b3 < lon0 || b1 > lon1);
        if (v2 && !want && fn) { p = end; continue; }          // RDM2 knows each polyline's length: skip it
        if (want) { la.resize(n > 0 ? n : 1); lo.resize(n > 0 ? n : 1); }
        if (n < 1 || !decode(d, p, end, n, a, o, mul, want ? la.data() : nullptr, want ? lo.data() : nullptr)) return -1;
        if (v2 && p != end) return -1;
        if (want && n >= 2) (*fn)(cls, la.data(), lo.data(), n);
    }
    return (long)count;
}

bool forEach(const uint8_t* d, size_t len, int lat0, int lon0, int lat1, int lon1,
             const std::function<void(int, const int32_t*, const int32_t*, int)>& fn, unsigned mask) {
    return walk(d, len, lat0, lon0, lat1, lon1, mask, &fn, nullptr) >= 0;
}

long check(const uint8_t* d, size_t len, std::string* desc) { return walk(d, len, 0, 0, 0, 0, 0, nullptr, desc); }

}  // namespace mapdata
