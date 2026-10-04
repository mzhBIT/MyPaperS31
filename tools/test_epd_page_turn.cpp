#include "lgfx/v1/platforms/esp32/EpdPageTurn.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace lgfx_epd_page_turn;

int main()
{
    static_assert(phases(15)==24);
    static_assert(phases(15,slow_spread)==35);
    static_assert(band_delay(0,slow_spread)==0 && band_delay(1,slow_spread)==6 &&
                  band_delay(2,slow_spread)==13 && band_delay(3,slow_spread)==20);
    static_assert(band_delay(0,slow_spread,8)==0 && band_delay(1,slow_spread,8)==3 &&
                  band_delay(2,slow_spread,8)==6 && band_delay(3,slow_spread,8)==9 &&
                  band_delay(4,slow_spread,8)==11 && band_delay(5,slow_spread,8)==14 &&
                  band_delay(6,slow_spread,8)==17 && band_delay(7,slow_spread,8)==20);
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
    for (int rotation=0;rotation<8;++rotation) {
        for (int direction : {-4,-3,-2,-1,1,2,3,4}) {
            std::vector<uint8_t> band_map(width*height/16);
            make_band_map(band_map.data(),width,height,rotation,direction);
            std::vector<Span> spans(height*bands);
            Layout layout;
            assert(prepare_layout(layout,width,height,rotation,direction,spans.data()));
            assert(layout.matches(width,height,rotation,direction));
            assert(!layout.matches(width,height,rotation,-direction));
            std::vector<uint8_t> fast(bytes);
            std::vector<int> drives(bytes,0);
            for (int phase=0;phase<phases(15);++phase) {
                compose_projected(out.data(),bytes,width,base.data(),map,15,phase,band_map.data());
                std::memset(fast.data(),0xA5,bytes); // Detect any unwritten neutral byte.
                clear_inactive_layout(fast.data(),bytes,15,phase,layout);
                copy_layout(fast.data(),bytes,base.data(),map,15,phase,layout);
                assert(fast==out);
                for (size_t at=0;at<bytes;++at) {
                    const int band=band_map[at/4];
                    assert(band>=0 && band<bands);
                    const int local=phase-band*stagger;
                    if (local>=0 && local<15) {
                        assert(out[at]==base[size_t(map[local])*bytes+at]);
                        ++drives[at];
                    } else assert(out[at]==0);
                }
            }
            for (int count : drives) assert(count==15);
            // Transform each logical corner into physical coordinates and
            // verify the earliest/latest projection for all mirror rotations.
            const int lw=(rotation&1)?height:width,lh=(rotation&1)?width:height;
            for (int corner=0;corner<4;++corner) {
                int x=(corner&1)?lw-1:0,y=(corner&2)?lh-1:0;
                const int lx=x,ly=y;
                if (rotation&1) std::swap(x,y);
                if ((1<<rotation)&0b11000110) x=width-1-x;
                if ((1<<rotation)&0b10011100) y=height-1-y;
                assert(projected_band(x,y,width,height,rotation,direction)==
                       projected_band(lx,ly,lw,lh,0,direction));
            }
        }
    }
    assert(base==unchanged);
    // Production-sized panel: verify fast paths and diagonal spans against
    // the previous group-by-group implementation, including mirror rotations.
    constexpr int pw=1216,ph=684;
    constexpr size_t pb=size_t(pw)*ph/4;
    std::vector<uint8_t> panel_base(pb*9),reference(pb),fast(pb),band_map(size_t(pw)*ph/16);
    for(size_t i=0;i<panel_base.size();++i) panel_base[i]=uint8_t(i*31+i/257);
    std::vector<Span> spans(ph*max_bands);
    for(int spread : {fast_spread,slow_spread})
    for(int r=0;r<8;++r) for(int d : {-4,-3,-2,-1,1,2,3,4}) {
        const int band_count=spread==slow_spread ? 8 : 4;
        std::vector<uint8_t> lazy(pb*9,0xA5);
        std::array<int,9> rendered{};
        constexpr int slots=12;
        std::vector<uint8_t> ring(pb*slots,0xA5);
        std::array<int,slots> tags;
        tags.fill(-1);
        int generated=0;
        std::array<int,max_bands> driven{};
        Layout layout;
        assert(prepare_layout(layout,pw,ph,r,d,spans.data(),band_count));
        assert(layout.matches(pw,ph,r,d,band_count));
        assert(!layout.matches(pw,ph,r,d,band_count==8 ? 4 : 8));
        make_band_map(band_map.data(),pw,ph,r,d,band_count);
        const auto produce=[&](int p) {
            const int slot=p%slots;
            assert(tags[slot]==-1); // No overwrite of queued/DMA-owned data.
            for_each_dependency(p,15,map,[&](int unique) {
                assert(unique>=0 && unique<9);
                if(!rendered[unique]) {
                    std::memcpy(lazy.data()+size_t(unique)*pb,panel_base.data()+size_t(unique)*pb,pb);
                    ++rendered[unique];
                }
            },spread,band_count);
            auto *out=ring.data()+size_t(slot)*pb;
            clear_inactive_layout(out,pb,15,p,layout,spread);
            copy_layout(out,pb,lazy.data(),map,15,p,layout,false,spread);
            tags[slot]=p;
        };
        for(;generated<8;++generated) produce(generated);
        for(int p=0;p<phases(15,spread);++p) {
            for(int band=0;band<band_count;++band) {
                const int local=p-band_delay(band,spread,band_count);
                if(local>=0 && local<15) ++driven[band];
            }
            // Fast producer fills available slots then must stop at the first
            // occupied slot. Consumer frees only the previous phase after DMA.
            while(generated<phases(15,spread) && tags[generated%slots]==-1) {
                produce(generated++);
            }
            assert(tags[p%slots]==p);
            compose_projected(reference.data(),pb,pw,panel_base.data(),map,15,p,band_map.data(),spread,band_count);
            std::memset(fast.data(),0xA5,pb);
            clear_inactive_layout(fast.data(),pb,15,p,layout,spread);
            copy_layout(fast.data(),pb,lazy.data(),map,15,p,layout,false,spread);
            assert(fast==reference);
            assert(std::memcmp(ring.data()+size_t(p%slots)*pb,reference.data(),pb)==0);
            if(p>0) {
                assert(tags[(p-1)%slots]==p-1);
                tags[(p-1)%slots]=-1;
            }
        }
        assert(generated==phases(15,spread));
        for(int band=0;band<max_bands;++band) assert(driven[band]==(band<band_count ? 15 : 0));
        for(int count:rendered) assert(count==1);
    }
    assert(local_phase(0,3,1)==0 && local_phase(0,0,-1)==0);
    std::puts("Page turn: fast 4/24, slow 8/35, eight directions, all rotations, neutral masks and ring equivalence passed");
}
