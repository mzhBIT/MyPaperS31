#pragma once

#include <cstdint>

namespace m5gfx {
class M5GFX;
}

namespace p4_jpeg {

struct DrawStats {
    bool attempted = false;
    bool decoded = false;
    bool scaled = false;
    int source_width = 0;
    int source_height = 0;
    int output_width = 0;
    int output_height = 0;
    int64_t read_ms = 0;
    int64_t header_ms = 0;
    int64_t decode_ms = 0;
    int64_t scale_ms = 0;
    int64_t push_ms = 0;
    int64_t total_ms = 0;
};

// Decodes a baseline color JPEG with the ESP32-P4 JPEG engine, scales/crops
// it into the requested box with PPA, and pushes RGB565 pixels to M5GFX.
// Returns false for unsupported images or resource failures so callers can
// transparently fall back to the software decoder.
bool draw_file(m5gfx::M5GFX &gfx,
               const char *path,
               int x,
               int y,
               int box_width,
               int box_height,
               DrawStats *stats = nullptr);

} // namespace p4_jpeg
