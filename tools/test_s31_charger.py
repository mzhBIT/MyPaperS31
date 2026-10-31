"""Generate native tests for the charger policy shared by both S31 images."""
from pathlib import Path
import sys

out=Path(sys.argv[1] if len(sys.argv)>1 else '.codex-build-cache/s31-charger')
out.mkdir(parents=True,exist_ok=True)
(out/'test.cpp').write_text(r'''
#include "boards/MyPaperS31/ChargerState.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace mypaper::board::s31::charger;
struct Bus {
    uint8_t r[256]{};
    int calls=0,fail=-1;
    bool ignore_otg=false;
    bool read(uint8_t reg,uint8_t& value) {
        if(calls++==fail)return false;
        value=r[reg];return true;
    }
    bool write(uint8_t reg,uint8_t value) {
        if(calls++==fail)return false;
        if(reg==Control1)assert(!(r[Control3]&EnableOtg));
        assert(reg==Control1 || reg==Control3);
        if(!(ignore_otg && reg==Control3))r[reg]=value;
        return true;
    }
    bool recover() {
        State a,b;
        return recover_for_boot([&](uint8_t r,uint8_t& v){return read(r,v);},
                                [&](uint8_t r,uint8_t v){return write(r,v);},a,b);
    }
};
int main() {
    for(unsigned c1=0;c1<256;++c1)for(unsigned c3=0;c3<256;++c3){
        Bus b;for(unsigned i=0;i<256;++i)b.r[i]=uint8_t(i*3);
        b.r[Control1]=c1;b.r[Control3]=c3;
        uint8_t initial[256];memcpy(initial,b.r,256);
        assert(b.recover());
        assert((b.r[Control3]&EnableOtg)==0);
        assert(b.r[Control1]==uint8_t((c1&~0x13)|0x20));
        for(unsigned i=0;i<256;++i)
            if(i!=Control1&&i!=Control3)assert(b.r[i]==initial[i]);
        initial[Control1]=b.r[Control1];initial[Control3]=b.r[Control3];
        assert(b.recover()&&!memcmp(initial,b.r,256));
    }
    for(int failure=0;failure<8;++failure){
        Bus b;b.r[Control1]=0x13;b.r[Control3]=EnableOtg;b.fail=failure;
        assert(!b.recover());
    }
    Bus stuck;stuck.r[Control3]=EnableOtg;stuck.ignore_otg=true;
    assert(!stuck.recover()&&!(stuck.r[Control1]&EnableCharge));
    assert(voltage_mv(0xc0,13)==4400);
    assert(charge_ma(0,3)==960);
    assert(input_ma(0x20,3)==1000);
    puts("S31 charger: 65536 retained-state combinations, unrelated limits preserved, idempotence, I2C failures, stuck OTG interlock and register decoding PASS");
}
''',encoding='utf-8')
print(f'Charger test ready: {out / "test.cpp"}')
