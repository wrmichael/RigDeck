// Outline map for the APRS screen: country, state/province and lake outlines packed by tools/make_map.py
// (Natural Earth 1:50m, public domain). Coordinates in 0.01 degree.
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

namespace mapdata {

enum Class { MC_COUNTRY = 0, MC_STATE = 1, MC_LAKE = 2 };

// Calls fn for each polyline whose bounding box meets lat0..lat1 / lon0..lon1 (0.01 degree). lat/lon hold n
// points. Returns false if the data is not a valid "RDM1" block.
bool forEach(const uint8_t* data, size_t len, int lat0, int lon0, int lat1, int lon1,
             const std::function<void(int cls, const int16_t* lat, const int16_t* lon, int n)>& fn);

}  // namespace mapdata
