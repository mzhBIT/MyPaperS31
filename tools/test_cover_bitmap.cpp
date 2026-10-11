#include "../main/renderer/CoverBitmapFormat.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace cover_thumbnail;
int main() {
    for (auto dimensions : {std::pair<unsigned,unsigned>{164,214},{120,156}}) {
        auto w=dimensions.first,h=dimensions.second;
        std::vector<uint8_t> pixels(w/2*h);
        for(size_t i=0;i<pixels.size();++i) pixels[i]=uint8_t(i*73);
        BitmapHeader header{0x34435643u,1,w,h,1234,5678,uint32_t(pixels.size()),
                            bitmap_checksum(pixels.data(),pixels.size())};
        assert(bitmap_header_valid(header,w,h,1234,5678));
        assert(!bitmap_header_valid(header,w+2,h,1234,5678));
        assert(!bitmap_header_valid(header,w,h,1235,5678));
        assert(!bitmap_header_valid(header,w,h,1234,5679));
        auto bad=header; bad.version++; assert(!bitmap_header_valid(bad,w,h,1234,5678));
        bad=header; bad.payload_bytes--; assert(!bitmap_header_valid(bad,w,h,1234,5678));
        bad=header; bad.magic=0; assert(!bitmap_header_valid(bad,w,h,1234,5678));
        pixels[1]^=1; assert(bitmap_checksum(pixels.data(),pixels.size())!=header.checksum);
    }
    // Verify capture through panel gray -> RGB565 -> gray4 is lossless.
    for(unsigned level=0;level<16;++level) {
        unsigned r5=(level*17)>>3;
        assert((r5*15+15)/31==level);
    }
    BitmapHeader zero{};
    assert(!bitmap_header_valid(zero,0,214,0,0));
    assert(!bitmap_header_valid(zero,163,214,0,0));
    puts("PASS: cover BIN dimensions, source identity, checksum and gray round trip");
}
