#include <cassert>
#include <cstdio>
#include "lgfx/v1/platforms/esp32/EpdChangedPixels.h"

int main() {
  // Exhaust all drive codes in all 256 old/new combinations. Unchanged pixels
  // must be neutral in every phase, not just when whole packed bytes match.
  for (unsigned drive = 0; drive < 4; ++drive) {
    uint8_t table[256];
    for (unsigned i = 0; i < 256; ++i) table[i] = (i + drive) % 4;
    lgfx_epd_changed_pixels::mask_unchanged(table);
    for (unsigned from = 0; from < 16; ++from)
      for (unsigned to = 0; to < 16; ++to) {
        const unsigned i = (from << 4) | to;
        assert(table[i] == (from == to ? 0 : (i + drive) % 4));
      }
  }
  std::puts("EPD changed-pixel mask: 16 neutral diagonals, 240 preserved transitions PASS");
}
