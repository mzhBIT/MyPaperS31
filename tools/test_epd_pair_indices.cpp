#include "lgfx/v1/platforms/esp32/EpdPairIndices.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>

int main() {
    std::vector<uint8_t> from(65536), to(65536), lut(65536);
    std::vector<uint16_t> indices(65536);
    // Exhaust every possible old/new pair key, including equal-gray cases.
    for (unsigned i = 0; i < 65536; ++i) {
        from[i] = uint8_t(i >> 8);
        to[i] = uint8_t(i);
    }
    lgfx_epd_pair_indices::build(indices.data(), from.data(), to.data(), indices.size());
    for (unsigned i = 0; i < 65536; ++i) assert(indices[i] == i);
    std::mt19937 random(307);
    static constexpr unsigned shifts[8] = {4, 0, 12, 8, 20, 16, 28, 24};
    for (unsigned frame = 0; frame < 48; ++frame) {
        for (auto& value : lut) value = uint8_t(random() & 15);
        for (size_t group = 0; group < indices.size() / 8; ++group) {
            uint32_t expected = 0;
            for (unsigned p = 0; p < 8; ++p) {
                const size_t i = group * 8 + p;
                expected |= uint32_t(lut[(unsigned(from[i]) << 8) | to[i]]) << shifts[p];
            }
            assert(lgfx_epd_pair_indices::group(indices.data() + group * 8, lut.data()) == expected);
        }
    }
    // Rebuild after the target changes: no stale indices from the prior batch.
    for (auto& value : to) value ^= 255;
    lgfx_epd_pair_indices::build(indices.data(), from.data(), to.data(), indices.size());
    for (unsigned i = 0; i < 65536; ++i)
        assert(indices[i] == ((unsigned(from[i]) << 8) | to[i]));
    // Exhaust small rectangles: non-16-pixel-aligned boundaries, one pair,
    // full rows, nonzero origins, and empty updates. Poison outside the area
    // to prove the renderer never uses stale indices there.
    constexpr size_t stride = 24, rows = 5;
    for (size_t y0 = 0; y0 <= rows; ++y0)
    for (size_t y1 = y0; y1 <= rows; ++y1)
    for (size_t x0 = 0; x0 <= stride; ++x0)
    for (size_t x1 = x0; x1 <= stride; ++x1) {
        std::vector<uint16_t> region(stride * rows, 0xffff);
        lgfx_epd_pair_indices::build_rows(region.data(), from.data(), to.data(),
                                         stride, x0, x1, y0, y1);
        for (size_t y = 0; y < rows; ++y) {
            for (size_t g = 0; g < stride / 8; ++g) {
                const size_t base = g * 8;
                uint32_t expected = 0, actual = 0;
                for (unsigned p = 0; p < 8; ++p) {
                    const size_t x = base + p, i = y * stride + x;
                    const bool inside = y >= y0 && y < y1 && x >= x0 && x < x1;
                    assert(region[i] == (inside ? ((unsigned(from[i]) << 8) | to[i]) : 0xffff));
                    if (inside)
                        expected |= uint32_t(lut[(unsigned(from[i]) << 8) | to[i]]) << shifts[p];
                }
                if (y >= y0 && y < y1 && base < x1 && base + 8 > x0) {
                    actual = lgfx_epd_pair_indices::group_range(region.data() + y * stride + base,
                        lut.data(), unsigned(std::max(base, x0) - base),
                        unsigned(std::min(base + 8, x1) - base));
                }
                assert(actual == expected);
            }
        }
    }
    std::puts("pair indices: exhaustive keys, 48 frame outputs and partial rectangles passed");
}
