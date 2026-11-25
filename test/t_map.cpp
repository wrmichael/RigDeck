// The packed outline maps read back whole: the built-in one (main/map_data.c) and, when it has been built, the
// detailed file (docs/map/rigdeck-map.rdm). Both find Indiana's outlines around EM69; damage is caught.
#include <cstdio>
#include <string>
#include <vector>
#include "mapdata.h"
#include "../main/map_data.c"

static int checks = 0, fails = 0;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static void around(const uint8_t* d, size_t len, int minStates, int maxStates, int* counties) {
    int near = 0, cty = 0;
    bool inBox = true;
    mapdata::forEach(d, len, 38000, -88000, 41000, -85000, [&](int c, const int32_t* la, const int32_t* lo, int n) {
        if (c == mapdata::MC_STATE) near++;
        if (c == mapdata::MC_COUNTY) cty++;
        for (int i = 0; i < n; i++) if (la[i] < -90000 || la[i] > 90000 || lo[i] < -180000 || lo[i] > 180000) inBox = false;
    });
    CHECK(near >= minStates && near < maxStates && inBox);       // Indiana and its neighbours, nothing far away
    if (counties) *counties = cty;
}

int main() {
    int all = 0, pts = 0, byCls[6] = {};
    bool ok = mapdata::forEach(g_mapData, g_mapDataLen, -90000, -180000, 90000, 180000, [&](int c, const int32_t*, const int32_t*, int n) {
        all++; pts += n; if (c >= 0 && c < 6) byCls[c]++;
    });
    printf("t_map: built in: %d polylines, %d points (countries %d, states %d, lakes %d), %u bytes\n", all, pts, byCls[0], byCls[1], byCls[2], (unsigned)g_mapDataLen);
    CHECK(ok && all > 2000 && byCls[0] > 500 && byCls[1] > 500 && byCls[2] > 10);
    CHECK(mapdata::check(g_mapData, g_mapDataLen) == all);
    around(g_mapData, g_mapDataLen, 2, 60, nullptr);
    int lakes = 0;                                                    // Lake Michigan is in
    mapdata::forEach(g_mapData, g_mapDataLen, 41500, -88000, 46000, -85000, [&](int, const int32_t*, const int32_t*, int) { lakes++; }, 1u << mapdata::MC_LAKE);
    CHECK(lakes >= 1);
    uint8_t bad[8] = {'X', 'X', 'X', 'X', 0, 0, 0, 0};
    CHECK(!mapdata::forEach(bad, sizeof bad, 0, 0, 1, 1, [](int, const int32_t*, const int32_t*, int) {}));
    CHECK(mapdata::check(g_mapData, g_mapDataLen - 7) < 0);          // cut off

    if (FILE* f = fopen("docs/map/rigdeck-map.rdm", "rb")) {
        std::vector<uint8_t> m;
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) m.insert(m.end(), buf, buf + n);
        fclose(f);
        std::string desc;
        long cnt = mapdata::check(m.data(), m.size(), &desc);
        int cty = 0;
        around(m.data(), m.size(), 2, 200, &cty);
        int roads = 0;
        mapdata::forEach(m.data(), m.size(), 39000, -87000, 40500, -85500, [&](int, const int32_t*, const int32_t*, int) { roads++; }, 1u << mapdata::MC_ROAD);
        printf("t_map: detailed: %ld polylines, %u bytes, %d county outlines and %d highways around Indianapolis; \"%s\"\n",
               cnt, (unsigned)m.size(), cty, roads, desc.c_str());
        CHECK(cnt > 20000 && desc.find("Natural Earth") != std::string::npos && cty > 50 && roads > 3);
        std::vector<uint8_t> cut(m.begin(), m.begin() + m.size() / 2);
        CHECK(mapdata::check(cut.data(), cut.size()) < 0);
    } else printf("t_map: docs/map/rigdeck-map.rdm not built, detailed map not checked\n");
    printf(fails ? "t_map: %d of %d FAILED\n" : "t_map: all %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
