#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace map_scale {
constexpr uint8_t online_min_zoom=3;
constexpr uint8_t online_max_zoom=19;
// Measured scale=1 AMap pixels are 2x XYZ at the same request zoom.
// Local nearest-neighbor adds another 2x, so request two levels lower.
inline uint8_t request_zoom(uint8_t display_zoom) {
    return uint8_t(std::max(1,std::min(17,int(display_zoom)-2)));
}
inline double world_pixels(uint8_t display_zoom) {
    return 256.0*std::pow(2.0,int(display_zoom));
}
}
