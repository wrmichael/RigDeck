#include "mapdata.h"
#include <cstring>
#include <vector>

namespace mapdata {

static int16_t rd16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }

bool forEach(const uint8_t* d, size_t len, int lat0, int lon0, int lat1, int lon1,
             const std::function<void(int, const int16_t*, const int16_t*, int)>& fn) {
    if (!d || len < 8 || memcmp(d, "RDM1", 4) != 0) return false;
    uint32_t count = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
    size_t p = 8;
    std::vector<int16_t> la, lo;
    for (uint32_t k = 0; k < count; k++) {
        if (p + 15 > len) return false;
        int cls = d[p];
        int n = d[p + 1] | (d[p + 2] << 8);
        int bLa0 = rd16(d + p + 3), bLo0 = rd16(d + p + 5), bLa1 = rd16(d + p + 7), bLo1 = rd16(d + p + 9);
        int a = rd16(d + p + 11), o = rd16(d + p + 13);
        p += 15;
        bool want = !(bLa1 < lat0 || bLa0 > lat1 || bLo1 < lon0 || bLo0 > lon1);
        if (want) { la.resize(n); lo.resize(n); la[0] = (int16_t)a; lo[0] = (int16_t)o; }
        for (int i = 1; i < n; i++) {
            if (p >= len) return false;
            int da, dd;
            if ((int8_t)d[p] == -128) {
                if (p + 5 > len) return false;
                da = rd16(d + p + 1); dd = rd16(d + p + 3); p += 5;
            } else {
                if (p + 2 > len) return false;
                da = (int8_t)d[p]; dd = (int8_t)d[p + 1]; p += 2;
            }
            a += da; o += dd;
            if (want) { la[i] = (int16_t)a; lo[i] = (int16_t)o; }
        }
        if (want && n >= 2) fn(cls, la.data(), lo.data(), n);
    }
    return true;
}

}  // namespace mapdata
