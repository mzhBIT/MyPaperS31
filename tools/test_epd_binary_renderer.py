"""Generate a native equivalence test using the actual panel render functions.

Compile the generated file with M5GFX/src and main include directories, once
with MYPAPER_EPD_OUTPUT_PERMUTE=0 and once with =1. No ESP hardware is needed.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'components/M5GFX/src/lgfx/v1/platforms/esp32/Panel_EPD_2D.cpp').read_text(encoding='utf-8')

def function(name):
    pos = source.index(name + '(')
    start = source.rfind('\n\n', 0, pos) + 2
    brace = source.index('{', pos)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

timing = source.index('  struct drive_frame_timing_t {')
program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#define __attribute__(args)
#include "lgfx/v1/platforms/esp32/EpdPairIndices.h"
static int64_t esp_timer_get_time() { return 0; }
'''
# Only copy the actual waveform arrays/initializer; their C descriptor structs
# use out-of-order designated initializers that native MSVC cannot compile.
du = (root / 'main/waveforms/my_waveform_du.h').read_text(encoding='utf-8')
start = du.index('const uint8_t my_du_25_0_data')
program += du[start:du.index('};', start) + 2] + '\n'
a2 = (root / 'main/waveforms/my_waveform_a2.h').read_text(encoding='utf-8')
end = a2.rfind('\n', 0, a2.index('const EpdWaveformPhases'))
program += a2[a2.index('#define MY_A2_PHASE_COUNT'):end] + '\n'
program += source[timing:source.index('};', timing) + 2] + '\n'
for name in ('waveform_drive', 'build_transition_lut', 'drive_pixel_pair',
             'build_gc16_pair_lut', 'permute_epd_output', 'render_phase_frame',
             'render_gc16_pair_phase_frame'):
    program += function(name)
program += r'''
static void check(const uint8_t* phase, unsigned width, unsigned height,
                  unsigned x,unsigned y,unsigned w,unsigned h,bool binary) {
    std::mt19937 random(431+binary);
    std::vector<uint8_t> from(width*height/2),to(from.size()),lut(65536);
    std::vector<uint32_t> reference(width*height/16+2,0xdeadbeef),actual=reference;
    uint8_t transition[256];
    build_transition_lut(transition,phase);
    build_gc16_pair_lut(lut.data(),transition);
    for(unsigned i=0;i<65536;++i)
        assert(lut[i]==drive_pixel_pair(uint8_t(i>>8),uint8_t(i),transition));
    for(auto& b:from) b=uint8_t(random());
    for(auto& b:to) b=binary ? uint8_t((random()&1 ? 0xf0 : 0)|(random()&1 ? 15 : 0)) : uint8_t(random());
    render_phase_frame(reinterpret_cast<uint8_t*>(reference.data()+1),width*height/4,
                       from.data(),to.data(),width,height,width,height,x,y,w,h,transition);
    render_gc16_pair_phase_frame(reinterpret_cast<uint8_t*>(actual.data()+1),
                       from.data(),to.data(),width,height,lut.data(),x,y,w,h);
    assert(reference==actual);
    assert(actual.front()==0xdeadbeef && actual.back()==0xdeadbeef);
}
int main() {
    my_a2_init();
    std::mt19937 random(805);
    for(unsigned mode=0;mode<2;++mode) {
        const unsigned count=mode ? MY_A2_PHASE_COUNT : 20;
        const uint8_t* table=mode ? &my_a2_25_0_data[0][0][0] : &my_du_25_0_data[0][0][0];
        for(unsigned phase=0;phase<count;++phase) {
            for(bool binary:{false,true}) {
                check(table+phase*64,1216,684,0,0,1216,684,binary);
                for(unsigned trial=0;trial<20;++trial) {
                    const unsigned x=random()%48,y=random()%5;
                    check(table+phase*64,48,5,x,y,random()%(49-x),random()%(6-y),binary);
                }
            }
        }
    }
    puts("DU/A2 actual phases: all pair keys, full/partial frames, gray/binary targets and sentinels PASS");
}
'''
output = Path(sys.argv[1])
output.write_text(program, encoding='utf-8')
print(output)
