#pragma once
#include <cstdint>

namespace lgfx_epd_du4 {
// Eight physical byte-pairs form one 16-pixel output group. Resolve the
// old/new index once and accumulate three independent, exact drive words.
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline void group3(const uint8_t* from, const uint8_t* to,
                   const uint8_t* codes, const uint8_t* lut0,
                   const uint8_t* lut1, const uint8_t* lut2,
                   uint32_t& word0, uint32_t& word1, uint32_t& word2) {
    static constexpr unsigned shifts[8] = {4, 0, 12, 8, 20, 16, 28, 24};
    word0 = word1 = word2 = 0;
    for (unsigned pair = 0; pair < 8; ++pair) {
        const unsigned index = (unsigned(from[pair]) << 4) | codes[to[pair]];
        const unsigned shift = shifts[pair];
        word0 |= uint32_t(lut0[index]) << shift;
        word1 |= uint32_t(lut1[index]) << shift;
        word2 |= uint32_t(lut2[index]) << shift;
    }
}
}
