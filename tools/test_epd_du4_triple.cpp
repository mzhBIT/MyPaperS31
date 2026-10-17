#include "lgfx/v1/platforms/esp32/EpdDu4Triple.h"
#include "lgfx/v1/platforms/esp32/EpdGc16Dual.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

static unsigned bucket(unsigned gray) {
    return gray <= 2 ? 0 : gray <= 7 ? 5 : gray <= 12 ? 10 : 15;
}
static uint32_t reference(const uint8_t* from, const uint8_t* to,
                          const uint8_t* transition) {
    static constexpr unsigned shifts[8] = {4, 0, 12, 8, 20, 16, 28, 24};
    uint32_t result = 0;
    for (unsigned pair = 0; pair < 8; ++pair) {
        const unsigned high = transition[(from[pair] & 0xf0) | bucket(to[pair] >> 4)];
        const unsigned low = transition[((from[pair] & 15) << 4) | bucket(to[pair] & 15)];
        result |= uint32_t((high << 2) | low) << shifts[pair];
    }
    return result;
}
int main() {
    std::mt19937 random(723);
    uint8_t tables[3][4096], transitions[3][256], codes[256], packed_lut[4096];
    static constexpr unsigned targets[4] = {0, 5, 10, 15};
    for (unsigned value = 0; value < 256; ++value) {
        auto code = [](unsigned gray) { return gray <= 2 ? 0 : gray <= 7 ? 1 : gray <= 12 ? 2 : 3; };
        codes[value] = uint8_t((code(value >> 4) << 2) | code(value & 15));
    }
    size_t checked = 0;
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        for (unsigned frame = 0; frame < 3; ++frame) {
            for (unsigned transition = 0; transition < 256; ++transition)
                transitions[frame][transition] = scenario < 4 ? uint8_t((scenario + frame) & 3)
                                                           : uint8_t(random() & 3);
            for (unsigned from = 0; from < 256; ++from)
                for (unsigned code = 0; code < 16; ++code) {
                    const unsigned high = transitions[frame][(from & 0xf0) | targets[code >> 2]];
                    const unsigned low = transitions[frame][((from & 15) << 4) | targets[code & 3]];
                    tables[frame][(from << 4) | code] = uint8_t((high << 2) | low);
                }
        }
        lgfx_epd_du4::build_dual_lut(packed_lut,transitions[0],transitions[1]);
        for(unsigned key=0;key<4096;++key) {
            assert((packed_lut[key]&15)==tables[0][key]);
            assert((packed_lut[key]>>4)==tables[1][key]);
        }
        // Every old/new byte pair, including unchanged pairs and all 16 grays.
        for (unsigned pair = 0; pair < 65536; ++pair) {
            uint8_t from[8], to[8];
            for (unsigned i = 0; i < 8; ++i) {
                from[i] = uint8_t((pair >> 8) + i * 17);
                to[i] = uint8_t(pair + i * 29);
            }
            uint32_t words[3];
            lgfx_epd_du4::group3(from, to, codes, tables[0], tables[1], tables[2],
                                 words[0], words[1], words[2]);
            for (unsigned frame = 0; frame < 3; ++frame)
                assert(words[frame] == reference(from, to, transitions[frame]));
            uint16_t indices[8];
            lgfx_epd_du4::build_indices(indices,from,to,codes,1,nullptr,1);
            for(unsigned i=0;i<8;++i)
                assert(indices[i]==((unsigned(from[i])<<4)|codes[to[i]]));
            const uint8_t* luts[]={tables[0],tables[1],tables[2]};
            uint32_t indexed[3],swapped[3];
            lgfx_epd_du4::group_indexed<3,false>(indices,luts,indexed);
            lgfx_epd_du4::group_indexed<3,true>(indices,luts,swapped);
            uint32_t direct_swapped[3];
            lgfx_epd_du4::group3<true>(from,to,codes,tables[0],tables[1],tables[2],
                direct_swapped[0],direct_swapped[1],direct_swapped[2]);
            for(unsigned frame=0;frame<3;++frame) {
                const uint32_t expected=((words[frame]&0xff00ffu)<<8)|((words[frame]&0xff00ff00u)>>8);
                assert(indexed[frame]==words[frame]);
                assert(swapped[frame]==expected && direct_swapped[frame]==expected);
            }
            uint32_t single;
            lgfx_epd_du4::group_indexed<1,true>(indices,luts,&single);
            assert(single==swapped[0]);
            const auto dual=lgfx_epd_gc16_dual::group<false,true>(indices,nullptr,nullptr,packed_lut);
            const auto dual_swapped=lgfx_epd_gc16_dual::group<true,true>(indices,nullptr,nullptr,packed_lut);
            assert(dual.first==words[0] && dual.second==words[1]);
            assert(dual_swapped.first==swapped[0] && dual_swapped.second==swapped[1]);
            ++checked;
        }
    }
    // Sparse indexing must not touch inactive groups, including at real screen size.
    std::vector<uint8_t> from(1216*684/2),to(from.size());
    std::vector<uint16_t> indices(from.size(),0xffff),active;
    for(auto& value:from) value=uint8_t(random());
    for(auto& value:to) value=uint8_t(random());
    const size_t groups=from.size()/8;
    for(size_t group=0;group<groups;++group)
        if(group%3==1) active.push_back(uint16_t(group));
    // Same chunking as the platform, including the short final chunk.
    for(size_t start=0;start<active.size();start+=256) {
        const size_t count=std::min<size_t>(256,active.size()-start);
        lgfx_epd_du4::build_indices(indices.data(),from.data(),to.data(),codes,
            count,active.data()+start,count);
    }
    for(size_t i=0;i<indices.size();++i)
        assert(indices[i]==(i/8%3==1?((unsigned(from[i])<<4)|codes[to[i]]):0xffff));
    for(size_t start=0;start<groups;start+=256) {
        const size_t count=std::min<size_t>(256,groups-start),offset=start*8;
        lgfx_epd_du4::build_indices(indices.data()+offset,from.data()+offset,to.data()+offset,
            codes,count,nullptr,count);
    }
    for(size_t i=0;i<indices.size();++i)
        assert(indices[i]==((unsigned(from[i])<<4)|codes[to[i]]));
    std::printf("DU4 triple-frame kernel: %zu groups match independent scalar waveform reference\n", checked);
}
