// The packed outline map (main/map_data.c) reads back whole and finds Indiana's outlines around EM69.
#include <cstdio>
#include "mapdata.h"
#include "../main/map_data.c"

static int checks = 0, fails = 0;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

int main() {
    int all = 0, pts = 0, byCls[3] = {};
    bool ok = mapdata::forEach(g_mapData, g_mapDataLen, -9000, -18000, 9000, 18000, [&](int c, const int16_t*, const int16_t*, int n) {
        all++; pts += n; if (c >= 0 && c < 3) byCls[c]++;
    });
    printf("t_map: %d polylines, %d points (countries %d, states %d, lakes %d), %u bytes\n", all, pts, byCls[0], byCls[1], byCls[2], (unsigned)g_mapDataLen);
    CHECK(ok && all > 2000 && byCls[0] > 500 && byCls[1] > 500 && byCls[2] > 5);
    int near = 0;
    bool inBox = true;
    mapdata::forEach(g_mapData, g_mapDataLen, 3800, -8800, 4100, -8500, [&](int c, const int16_t* la, const int16_t* lo, int n) {
        if (c == mapdata::MC_STATE) near++;
        for (int i = 0; i < n; i++) if (la[i] < -9000 || la[i] > 9000 || lo[i] < -18000 || lo[i] > 18000) inBox = false;
    });
    CHECK(near >= 2 && near < 60 && inBox);                         // Indiana and its neighbours, nothing far away
    uint8_t bad[8] = {'X', 'X', 'X', 'X', 0, 0, 0, 0};
    CHECK(!mapdata::forEach(bad, sizeof bad, 0, 0, 1, 1, [](int, const int16_t*, const int16_t*, int) {}));
    printf(fails ? "t_map: %d of %d FAILED\n" : "t_map: all %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
