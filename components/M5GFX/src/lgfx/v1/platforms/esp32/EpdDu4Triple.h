#pragma once
#include <cstdint>
#include <cstddef>
#include "EpdPairIndices.h"

namespace lgfx_epd_du4 {
// Eight physical byte-pairs form one 16-pixel output group. Resolve the
// old/new index once and accumulate three independent, exact drive words.
template<bool SwapBytes = false>
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline void group3(const uint8_t* from, const uint8_t* to,
                   const uint8_t* codes, const uint8_t* lut0,
                   const uint8_t* lut1, const uint8_t* lut2,
                   uint32_t& word0, uint32_t& word1, uint32_t& word2) {
    const auto& shifts = lgfx_epd_pair_indices::output_shifts<SwapBytes>;
    word0 = word1 = word2 = 0;
    for (unsigned pair = 0; pair < 8; ++pair) {
        const unsigned index = (unsigned(from[pair]) << 4) | codes[to[pair]];
        const unsigned shift = shifts[pair];
        word0 |= uint32_t(lut0[index]) << shift;
        word1 |= uint32_t(lut1[index]) << shift;
        word2 |= uint32_t(lut2[index]) << shift;
    }
}

// A DU4 key needs 12 bits: old byte (8) + quantized target pair (4).
// Only active groups are initialized in the sparse path; never read the rest.
#if defined(__GNUC__)
__attribute__((optimize("O3")))
#endif
inline void build_indices(uint16_t* indices,const uint8_t* from,const uint8_t* to,
                          const uint8_t* codes,size_t groups,
                          const uint16_t* active,size_t active_count) {
    const size_t count=active?active_count:groups;
    for(size_t i=0;i<count;++i) {
        const size_t base=size_t(active?active[i]:i)<<3;
        for(unsigned pair=0;pair<8;++pair)
            indices[base+pair]=uint16_t((unsigned(from[base+pair])<<4)|codes[to[base+pair]]);
    }
}

template<unsigned Count,bool SwapBytes>
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline void group_indexed(const uint16_t* indices,const uint8_t* const* luts,
                          uint32_t* words) {
    for(unsigned frame=0;frame<Count;++frame) words[frame]=0;
    for(unsigned pair=0;pair<8;++pair) {
        const unsigned key=indices[pair];
        for(unsigned frame=0;frame<Count;++frame)
            words[frame]|=uint32_t(luts[frame][key])<<lgfx_epd_pair_indices::output_shifts<SwapBytes>[pair];
    }
}

// Pack two drive nibbles into the SAME 4096-byte LUT. Codes remain at +4096.
inline void build_dual_lut(uint8_t* lut,const uint8_t* first,const uint8_t* second) {
    constexpr unsigned targets[4]={0,5,10,15};
    for(unsigned from=0;from<256;++from)
    for(unsigned code=0;code<16;++code) {
        const unsigned hi=(from&0xf0)|targets[code>>2];
        const unsigned lo=((from&15)<<4)|targets[code&3];
        lut[(from<<4)|code]=uint8_t((first[hi]<<2)|first[lo]|(second[hi]<<6)|(second[lo]<<4));
    }
}
}
