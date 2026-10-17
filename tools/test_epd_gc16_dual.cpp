#include "lgfx/v1/platforms/esp32/EpdGc16Dual.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>

static uint8_t pair_drive(uint8_t from,uint8_t to,const uint8_t* transition) {
    return uint8_t((transition[(from&0xf0)|(to>>4)]<<2) |
        transition[((from&15)<<4)|(to&15)]);
}

template<bool SwapBytes>
static void check_rect(const std::vector<uint8_t>& lut,const uint8_t* phase0,const uint8_t* phase1,
                       const std::vector<uint8_t>& from,const std::vector<uint8_t>& to,
                       size_t width,size_t rows,unsigned x,unsigned y,unsigned w,unsigned h) {
    const size_t stride=width/2,words=width*rows/16;
    std::vector<uint16_t> indices(stride*rows,0xffff);
    const size_t begin=x/2,end=std::min(stride,size_t(x+w)/2);
    const size_t y_end=std::min(rows,size_t(y+h));
    lgfx_epd_pair_indices::build_rows(indices.data(),from.data(),to.data(),stride,begin,end,y,y_end);
    std::vector<uint32_t> first(words+2,0xdeadbeef),second(words+2,0xdeadbeef);
    std::vector<uint32_t> direct0(words+2,0xdeadbeef),direct1(words+2,0xdeadbeef);
    lgfx_epd_gc16_dual::render<SwapBytes,true>(reinterpret_cast<uint8_t*>(first.data()+1),
        reinterpret_cast<uint8_t*>(second.data()+1),from.data(),to.data(),indices.data(),lut.data(),
        width,rows,x,y,w,h);
    lgfx_epd_gc16_dual::render<SwapBytes,false>(reinterpret_cast<uint8_t*>(direct0.data()+1),
        reinterpret_cast<uint8_t*>(direct1.data()+1),from.data(),to.data(),nullptr,lut.data(),
        width,rows,x,y,w,h);
    constexpr unsigned shifts[]={4,0,12,8,20,16,28,24};
    for(size_t row=0;row<rows;++row)
    for(size_t group=0;group<width/16;++group) {
        uint32_t expected0=0,expected1=0;
        for(unsigned pair=0;pair<8;++pair) {
            const size_t byte=group*8+pair,offset=row*stride+byte;
            if(row<y || row>=y_end || byte<begin || byte>=end) continue;
            expected0|=uint32_t(pair_drive(from[offset],to[offset],phase0))<<shifts[pair];
            expected1|=uint32_t(pair_drive(from[offset],to[offset],phase1))<<shifts[pair];
        }
        if(SwapBytes) {
            expected0=((expected0&0xff00ffu)<<8)|((expected0&0xff00ff00u)>>8);
            expected1=((expected1&0xff00ffu)<<8)|((expected1&0xff00ff00u)>>8);
        }
        const size_t output=1+row*(width/16)+group;
        assert(first[output]==expected0 && second[output]==expected1);
    }
    assert(first==direct0 && second==direct1);
    // Integration may time the clears separately and pass precleared=true.
    std::fill(direct0.begin()+1,direct0.end()-1,0);
    std::fill(direct1.begin()+1,direct1.end()-1,0);
    lgfx_epd_gc16_dual::render<SwapBytes,true>(reinterpret_cast<uint8_t*>(direct0.data()+1),
        reinterpret_cast<uint8_t*>(direct1.data()+1),from.data(),to.data(),indices.data(),lut.data(),
        width,rows,x,y,w,h,true);
    assert(first==direct0 && second==direct1);
    assert(first.front()==0xdeadbeef && first.back()==0xdeadbeef);
    assert(second.front()==0xdeadbeef && second.back()==0xdeadbeef);
}

static void check_slots() {
    // Exhaust all 4-slot resident layouts, consumption points and exclusions.
    // Frame 0 is used again late: its first scan must NOT release its slot.
    const uint16_t last[] = {7,1,5,3};
    for(int code=0;code<625;++code) {
        int residents[4],value=code;
        for(auto& resident:residents) { resident=value%5-1; value/=5; }
        for(int consumed=-1;consumed<=8;++consumed)
        for(int excluded=-1;excluded<4;++excluded) {
            const auto find=[&](int exclude) {
                return lgfx_epd_gc16_dual::reusable_slot(4,consumed,last,
                    [&](int slot) { return residents[slot]; },exclude);
            };
            int expected=-1;
            for(int slot=0;slot<4;++slot) {
                if(slot==excluded) continue;
                if(residents[slot]>=0 && last[residents[slot]]>consumed) continue;
                expected=slot; break;
            }
            assert(find(excluded)==expected);
            if(excluded!=-1) continue;
            const int first=find(-1);
            if(first<0) continue;
            const int saved=residents[first];
            residents[first]=-1; // Producer reservation uses explicit exclusion.
            const int second=find(first);
            assert(second!=first);
            if(second>=0)
                assert(residents[second]<0 || last[residents[second]]<=consumed);
            // No second slot means single-frame fallback, not a wait.
            residents[first]=saved;
        }
    }
}

int main() {
    check_slots();
    std::mt19937 random(20261017);
    std::vector<uint8_t> lut(65536),phase0(256),phase1(256);
    for(unsigned repeat=0;repeat<48;++repeat) {
        for(auto& value:phase0) value=uint8_t(random()&3);
        for(auto& value:phase1) value=uint8_t(random()&3);
        lgfx_epd_gc16_dual::build_lut(lut.data(),phase0.data(),phase1.data());
        for(unsigned from=0;from<256;++from)
        for(unsigned to=0;to<256;++to) {
            const uint8_t packed=lut[(from<<8)|to];
            assert((packed&15)==pair_drive(from,to,phase0.data()));
            assert((packed>>4)==pair_drive(from,to,phase1.data()));
        }
    }
    constexpr unsigned width=48,rows=5;
    std::vector<uint8_t> from(width*rows/2),to(from.size());
    for(auto& value:from) value=uint8_t(random());
    for(auto& value:to) value=uint8_t(random());
    for(unsigned y=0;y<=rows;++y)
    for(unsigned h=0;h<=rows-y;++h)
    for(unsigned x=0;x<=width;++x)
    for(unsigned w=0;w<=width-x;++w) {
        check_rect<false>(lut,phase0.data(),phase1.data(),from,to,width,rows,x,y,w,h);
        check_rect<true>(lut,phase0.data(),phase1.data(),from,to,width,rows,x,y,w,h);
    }
    // One real full-screen pair as well as all small odd/even clipped rectangles.
    from.resize(1216*684/2);to.resize(from.size());
    for(auto& value:from) value=uint8_t(random());
    for(auto& value:to) value=uint8_t(random());
    check_rect<true>(lut,phase0.data(),phase1.data(),from,to,1216,684,0,0,1216,684);
    std::puts("GC16 dual: ring slot lifetimes/exclusions, exhaustive LUTs, indexed/direct paths, full/partial frames and byte orders PASS");
}
