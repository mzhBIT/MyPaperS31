#include "lgfx/v1/platforms/esp32/EpdPageTurn.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace lgfx_epd_page_turn;

int main()
{
    static_assert(phases(15)==24);
    constexpr size_t width=64, height=36, bytes=width*height/4;
    std::vector<uint8_t> base(bytes*9);
    for (int i=0; i<9; ++i)
        for (size_t b=0; b<bytes; ++b) base[size_t(i)*bytes+b]=uint8_t(1+(i*17+b)%254);
    const auto unchanged=base;
    uint16_t map[15];
    for (int i=0; i<15; ++i) map[i]=uint16_t(i%9);
    std::vector<uint8_t> out(bytes);
    for (int rotation=0; rotation<8; ++rotation) {
        Rect rects[bands];
        for (int band=0; band<bands; ++band) {
            const int lw=(rotation&1) ? height : width;
            const int lh=(rotation&1) ? width : height;
            int x=band*lw/bands, ex=(band+1)*lw/bands-1, y=0, ey=lh-1;
            if (rotation&1) { std::swap(x,y); std::swap(ex,ey); }
            if ((1<<rotation)&0b11000110) { const int old=x; x=width-1-ex; ex=width-1-old; }
            if ((1<<rotation)&0b10011100) { const int old=y; y=height-1-ey; ey=height-1-old; }
            rects[band]={uint16_t(x),uint16_t(y),uint16_t(ex-x+1),uint16_t(ey-y+1)};
        }
        assert(valid_rects(rects,width,height));
        for (int direction : {-1,1}) {
            std::vector<int> drives(bytes,0);
            for (int phase=0; phase<24; ++phase) {
                compose(out.data(),bytes,width,base.data(),map,15,phase,direction,rects);
                for (int band=0; band<bands; ++band) {
                    const int local=local_phase(phase,band,direction);
                    const auto &r=rects[band];
                    for (size_t y=r.y; y<size_t(r.y)+r.h; ++y)
                        for (size_t b=r.x/4; b<size_t(r.x+r.w)/4; ++b) {
                            const size_t at=y*(width/4)+b;
                            if (local>=0 && local<15) {
                                assert(out[at]==base[size_t(map[local])*bytes+at]);
                                ++drives[at];
                            } else assert(out[at]==0);
                        }
                }
            }
            for (int count : drives) assert(count==15);
        }
        rects[1]=rects[0];
        assert(!valid_rects(rects,width,height));
    }
    assert(base==unchanged);
    assert(local_phase(0,3,1)==0 && local_phase(0,0,-1)==0);
    std::puts("Page turn: all rotations, both directions, dedup mapping, 15 phases/pixel and neutral masks passed");
}
