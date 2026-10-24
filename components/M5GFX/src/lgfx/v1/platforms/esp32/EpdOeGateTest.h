#pragma once
#include <cstddef>
#include <cstdint>

namespace lgfx_epd_oe_test {
struct Pulse { uint16_t ticks; uint8_t level; };
// Experimental coarse row gate: preserve eight guard rows on each side.
// Three CKV startup rows approximate the source-latch pipeline latency.
// This is NOT a calibrated arbitrary-rectangle clipping implementation.
inline constexpr size_t build(Pulse* out, size_t capacity, uint32_t rows,
                    uint32_t row_ticks, uint32_t first, uint32_t end) {
    if (!out || !row_ticks || first >= end || end > rows) return 0;
    const uint32_t total = (rows + 5) * row_ticks;
    const uint32_t begin = first > 8 ? (first - 8 + 3) * row_ticks : 0;
    const uint32_t finish = end + 8 < rows ? (end + 8 + 3) * row_ticks : total;
    size_t n = 0;
    auto append = [&](uint32_t ticks, uint8_t level) {
        while (ticks) {
            if (n >= capacity) return false;
            const uint16_t chunk = ticks > 32767 ? 32767 : ticks;
            out[n++] = {chunk, level};
            ticks -= chunk;
        }
        return true;
    };
    if (!append(begin, 0) || !append(finish-begin, 1) || !append(total-finish, 0)) return 0;
    // An even number of nonzero half-symbols avoids an early RMT stop marker.
    if (n & 1) { if (n >= capacity) return 0; out[n++] = {1, 0}; }
    return n;
}
}
