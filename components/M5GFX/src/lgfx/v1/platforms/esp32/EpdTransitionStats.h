#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace lgfx_epd_stats {

// Preserve the existing byte-rounded rectangle, including both nibbles at
// odd pixel edges. This is conservative for phase deduplication.
#if defined(__GNUC__)
__attribute__((optimize("O3")))
#endif
inline size_t collect_rect(bool* used, const uint8_t* physical,
                           const uint8_t* target, size_t stride, size_t height,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    std::memset(used, 0, 256 * sizeof(*used));
    const size_t begin = std::min<size_t>(x >> 1, stride);
    const size_t end = std::min<size_t>((size_t(x) + w + 1) >> 1, stride);
    const size_t first_row = std::min<size_t>(y, height);
    const size_t last_row = std::min<size_t>(size_t(y) + h, height);
    for (size_t row = first_row; row < last_row; ++row) {
        const uint8_t* from_row = physical + row * stride;
        const uint8_t* to_row = target + row * stride;
        for (size_t i = begin; i < end; ++i) {
            const uint8_t from = from_row[i];
            const uint8_t to = to_row[i];
            used[(from & 0xF0) | (to >> 4)] = true;
            used[((from & 0x0F) << 4) | (to & 0x0F)] = true;
        }
    }
    size_t count = 0;
    for (size_t i = 0; i < 256; ++i) count += used[i];
    return count;
}

// Full-screen packed 4bpp input, eight bytes (16 pixels) per scan group.
// Keep exact 16-level transitions: equal grays may still require drive.
#if defined(__GNUC__)
__attribute__((optimize("O3")))
#endif
inline size_t collect(bool* used, uint16_t* groups, size_t& group_count,
                      const uint8_t* physical, const uint8_t* target,
                      size_t bytes, const bool* driven)
{
    // A 256-byte table combines "requires drive" (bit 0) and "seen"
    // (bit 1). No per-pixel conditional insertion/counting, and no second
    // lookup in a separate driven table. Count distinct transitions once
    // after the image traversal instead.
    uint8_t flags[256];
    for (size_t i = 0; i < 256; ++i) flags[i] = driven[i] ? 1 : 0;
    group_count = 0;
    for (size_t group = 0; group < bytes / 8; ++group) {
        uint8_t active = 0;
        const size_t base = group * 8;
        for (unsigned pair = 0; pair < 8; ++pair) {
            const uint8_t from = physical[base + pair];
            const uint8_t to = target[base + pair];
            const uint8_t high = (from & 0xF0) | (to >> 4);
            const uint8_t low = ((from & 0x0F) << 4) | (to & 0x0F);
            const uint8_t high_flags = flags[high];
            const uint8_t low_flags = flags[low];
            flags[high] = high_flags | 2;
            flags[low] = low_flags | 2;
            active |= high_flags | low_flags;
        }
        if (active & 1) groups[group_count++] = uint16_t(group);
    }
    size_t transitions = 0;
    for (size_t i = 0; i < 256; ++i) {
        used[i] = (flags[i] & 2) != 0;
        transitions += used[i];
    }
    return transitions;
}

} // namespace lgfx_epd_stats
