#include "lgfx/v1/platforms/esp32/EpdTransitionStats.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

int main()
{
    std::mt19937 rng(20261004);
    for (int test = 0; test < 336; ++test) {
        const size_t bytes = test == 79 ? 415872 : size_t(test + 1) * 8;
        std::vector<uint8_t> from(bytes), to(bytes);
        std::array<bool, 256> driven{}, expected{}, actual{};
        for (size_t i = 0; i < 256; ++i)
            driven[i] = test % 4 == 0 ? false : test % 4 == 1 ? true : (rng() % 3 == 0);
        for (size_t i = 0; i < bytes; ++i) {
            from[i] = uint8_t(rng());
            to[i] = test % 5 == 0 ? from[i] : uint8_t(rng());
            if (test >= 80) {
                const unsigned transition = unsigned(test - 80);
                from[i] = uint8_t((transition >> 4) * 17);
                to[i] = uint8_t((transition & 15) * 17);
            }
            for (unsigned shift : {0u, 4u})
                expected[((from[i] >> shift) & 15) * 16 + ((to[i] >> shift) & 15)] = true;
        }
        std::vector<uint16_t> reference, groups(bytes / 8);
        for (size_t group = 0; group < bytes / 8; ++group) {
            bool active = false;
            for (size_t i = group * 8; i < group * 8 + 8; ++i)
                for (unsigned shift : {0u, 4u})
                    active |= driven[((from[i] >> shift) & 15) * 16 + ((to[i] >> shift) & 15)];
            if (active) reference.push_back(uint16_t(group));
        }
        size_t count = 0;
        const size_t transitions = lgfx_epd_stats::collect(actual.data(), groups.data(), count,
                                                         from.data(), to.data(), bytes, driven.data());
        assert(actual == expected);
        size_t expected_count = 0;
        for (bool used : expected) expected_count += used;
        assert(transitions == expected_count);
        // The common non-fused path must match full-screen statistics too.
        std::array<bool, 256> rectangle{};
        assert(lgfx_epd_stats::collect_rect(rectangle.data(), from.data(), to.data(),
                    bytes, 1, 0, 0, uint16_t(std::min<size_t>(bytes * 2, 65535)), 1)
               <= 256);
        if (bytes * 2 <= 65535) assert(rectangle == expected);
        groups.resize(count);
        assert(groups == reference);
    }
    // Reference the old conditional insertion algorithm for clipped/odd/
    // empty rectangles and deliberately out-of-bounds rectangle coordinates.
    for (int test = 0; test < 512; ++test) {
        const size_t stride = 24, height = 17;
        std::vector<uint8_t> from(stride * height), to(stride * height);
        for (size_t i = 0; i < from.size(); ++i) {
            from[i] = uint8_t(rng()); to[i] = uint8_t(rng());
        }
        const uint16_t x = uint16_t(rng() % 64), y = uint16_t(rng() % 24);
        const uint16_t w = uint16_t(rng() % 64), h = uint16_t(rng() % 24);
        std::array<bool, 256> expected{}, actual{};
        size_t count = 0;
        for (size_t row = std::min<size_t>(y, height);
             row < std::min<size_t>(size_t(y) + h, height); ++row) {
            for (size_t i = std::min<size_t>(x >> 1, stride);
                 i < std::min<size_t>((size_t(x) + w + 1) >> 1, stride); ++i) {
                for (unsigned shift : {0u, 4u}) {
                    const unsigned type = ((from[row * stride + i] >> shift) & 15) * 16
                                        + ((to[row * stride + i] >> shift) & 15);
                    if (!expected[type]) { expected[type] = true; ++count; }
                }
            }
        }
        assert(lgfx_epd_stats::collect_rect(actual.data(), from.data(), to.data(),
                                            stride, height, x, y, w, h) == count);
        assert(actual == expected);
    }
    std::puts("EPD fused transition/group statistics: passed (336 cases, all 256 transitions)");
    std::puts("EPD common rectangle statistics: passed (512 boundary cases)");
}
