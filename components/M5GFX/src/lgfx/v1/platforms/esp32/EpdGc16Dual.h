#pragma once
#include "EpdPairIndices.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lgfx_epd_gc16_dual {
// The sole producer reserves slots; the consumer only advances consumed_phase.
// A repeated frame stays resident until its LAST scan phase has completed.
template<class ResidentAt>
inline int reusable_slot(int slots,int consumed,const uint16_t* last_phase,
                         ResidentAt resident_at,int excluded=-1) {
    for(int slot=0;slot<slots;++slot) {
        if(slot==excluded) continue;
        const int resident=resident_at(slot);
        if(resident<0 || consumed>=last_phase[resident]) return slot;
    }
    return -1;
}

// Two phases fit in the existing byte LUT: one 4-bit pair drive per nibble.
// Input transition tables must contain only 2-bit drive values.
#if defined(__GNUC__)
__attribute__((optimize("O3")))
#endif
inline void build_lut(uint8_t* lut,const uint8_t* first,const uint8_t* second) {
#if defined(__GNUC__)
    // The panel's reusable LUT is 64-byte aligned; retain word stores through memcpy.
    lut=static_cast<uint8_t*>(__builtin_assume_aligned(lut,4));
#endif
    uint8_t high[16][16];
    uint32_t low[16][4];
    for(unsigned old=0;old<16;++old) {
        for(unsigned target=0;target<16;++target)
            high[old][target]=uint8_t((first[(old<<4)|target]<<2) |
                                      (second[(old<<4)|target]<<6));
        for(unsigned block=0;block<4;++block) {
            uint32_t word=0;
            for(unsigned lane=0;lane<4;++lane) {
                const unsigned key=(old<<4)|(block<<2)|lane;
                word|=uint32_t(first[key] | (second[key]<<4))<<(lane*8);
            }
            low[old][block]=word;
        }
    }
    for(unsigned from=0;from<256;++from) {
        for(unsigned target_high=0;target_high<16;++target_high) {
            const uint32_t high_word=uint32_t(high[from>>4][target_high])*0x01010101u;
            for(unsigned block=0;block<4;++block) {
                const uint32_t word=high_word | low[from&15][block];
                // memcpy permits host tests and avoids alignment/aliasing assumptions.
                std::memcpy(lut+(from<<8)+(target_high<<4)+(block<<2),&word,4);
            }
        }
    }
}

struct Group { uint32_t first=0,second=0; };
template<bool SwapBytes,bool Indexed>
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline Group group(const uint16_t* indices,const uint8_t* from,const uint8_t* to,
                   const uint8_t* lut,unsigned first=0,unsigned end=8) {
    Group result;
    for(unsigned pair=first;pair<end;++pair) {
        const unsigned index=Indexed?indices[pair]:((unsigned(from[pair])<<8)|to[pair]);
        const uint32_t drive=lut[index];
        const unsigned shift=lgfx_epd_pair_indices::output_shifts<SwapBytes>[pair];
        result.first|=(drive&15)<<shift;
        result.second|=(drive>>4)<<shift;
    }
    return result;
}

// Output remains one complete neutral-padded scan frame per phase. No ring or
// physical-state ownership changes are needed for paired generation.
template<bool SwapBytes,bool Indexed>
#if defined(__GNUC__)
__attribute__((optimize("O3")))
#endif
inline void render(uint8_t* first_frame,uint8_t* second_frame,
                   const uint8_t* physical,const uint8_t* target,
                   const uint16_t* indices,const uint8_t* lut,
                   size_t memory_width,size_t rows,
                   uint16_t x,uint16_t y,uint16_t width,uint16_t height,
                   bool precleared=false) {
    const size_t stride=memory_width>>1,output_stride=memory_width>>2;
    const bool full=x==0 && y==0 && width==memory_width && height==rows;
    const size_t byte_begin=full?0:std::min(stride,size_t(x)>>1);
    const size_t byte_end=full?stride:std::min(stride,(size_t(x)+width)>>1);
    const size_t y_begin=full?0:std::min(rows,size_t(y));
    const size_t y_end=full?rows:std::min(rows,size_t(y)+height);
    if(full) {
        for(size_t row=0;row<rows;++row) {
            auto* first_output=reinterpret_cast<uint32_t*>(first_frame+row*output_stride);
            auto* second_output=reinterpret_cast<uint32_t*>(second_frame+row*output_stride);
            for(size_t g=0;g<memory_width/16;++g) {
                const size_t offset=row*stride+(g<<3);
                const auto drives=group<SwapBytes,Indexed>(Indexed?indices+offset:nullptr,
                    Indexed?nullptr:physical+offset,Indexed?nullptr:target+offset,lut);
                first_output[g]=drives.first;
                second_output[g]=drives.second;
            }
        }
        return;
    }
    if(!full && !precleared) {
        std::memset(first_frame,0,output_stride*rows);
        std::memset(second_frame,0,output_stride*rows);
    }
    for(size_t row=y_begin;row<y_end;++row) {
        auto* first_output=reinterpret_cast<uint32_t*>(first_frame+row*output_stride);
        auto* second_output=reinterpret_cast<uint32_t*>(second_frame+row*output_stride);
        for(size_t g=byte_begin>>3;g<(byte_end+7)>>3;++g) {
            const size_t base=g<<3,offset=row*stride+base;
            const uint16_t* index_group=Indexed?indices+offset:nullptr;
            const uint8_t* from=Indexed?nullptr:physical+offset;
            const uint8_t* to=Indexed?nullptr:target+offset;
            Group drives;
            if(base>=byte_begin && base+8<=byte_end)
                drives=group<SwapBytes,Indexed>(index_group,from,to,lut);
            else
                drives=group<SwapBytes,Indexed>(index_group,from,to,lut,
                    unsigned(std::max(base,byte_begin)-base),
                    unsigned(std::min(base+8,byte_end)-base));
            first_output[g]=drives.first;
            second_output[g]=drives.second;
        }
    }
}
}
