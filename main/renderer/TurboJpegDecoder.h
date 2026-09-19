#pragma once

#include <cstdint>
#include <cstddef>

namespace lgfx {
inline namespace v1 {
class LGFXBase;
}
}

namespace turbo_jpeg {

struct DrawStats {
    bool attempted = false;
    bool progressive = false;
    int source_width = 0;
    int source_height = 0;
    int decoded_width = 0;
    int decoded_height = 0;
    int scale_denom = 1;
    int64_t header_ms = 0;
    int64_t decode_ms = 0;
    int64_t push_ms = 0;
    int64_t total_ms = 0;
    uint32_t memory_budget = 0;
    uint32_t legacy_coefficient_bytes = 0;
    uint32_t compact_coefficient_bytes = 0;
};

// Software JPEG fallback with progressive support. The decoder uses scaled
// IDCT and scanline output, and caps libjpeg's memory manager so oversized
// progressive coefficient buffers fail without exhausting PSRAM.
bool draw_file(lgfx::LGFXBase &gfx,
               const char *path,
               int x,
               int y,
               int box_width,
               int box_height,
               DrawStats *stats = nullptr);

bool draw_memory(lgfx::LGFXBase &gfx,
                 const uint8_t *data,
                 size_t size,
                 const char *label,
                 int x,
                 int y,
                 int box_width,
                 int box_height,
                 DrawStats *stats = nullptr);

} // namespace turbo_jpeg
