#pragma once
#include <stdint.h>

namespace lgfx_epd_changed_pixels {
// Drive code 0 is neutral. Keep every changing-gray transition intact.
inline void mask_unchanged(uint8_t* transitions) {
  for (unsigned gray = 0; gray < 16; ++gray)
    transitions[(gray << 4) | gray] = 0;
}
}
