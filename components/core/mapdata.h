// Outline maps for the APRS screen, packed by tools/make_map.py from Natural Earth (public domain):
// "RDM1" built into the firmware (1:50m, 0.01 degree), "RDM2" the detailed file on the microSD card (1:10m,
// 0.001 degree, with US counties and highways). Both are read through forEach in 0.001 degree.
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

namespace mapdata {

enum Class { MC_COUNTRY = 0, MC_STATE = 1, MC_LAKE = 2, MC_COUNTY = 3, MC_ROAD = 4, MC_ROAD2 = 5 };

// Calls fn for each polyline whose bounding box meets lat0..lat1 / lon0..lon1 (0.001 degree) and whose class bit
// is set in classMask. lat/lon hold n points in 0.001 degree. Returns false if the data is not a valid map.
bool forEach(const uint8_t* data, size_t len, int lat0, int lon0, int lat1, int lon1,
             const std::function<void(int cls, const int32_t* lat, const int32_t* lon, int n)>& fn,
             unsigned classMask = 0xff);

// Checks a whole map (both formats): every polyline complete and inside the file. desc gets the RDM2
// description ("" for RDM1). Returns the number of polylines, or -1 if damaged.
long check(const uint8_t* data, size_t len, std::string* desc = nullptr);

}  // namespace mapdata
