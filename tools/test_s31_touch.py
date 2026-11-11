"""Host regression tests of the actual recognizer and S31 FT6336 frame decoder."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.codex-build-cache/touch-regression'
out.mkdir(parents=True, exist_ok=True)

stubs = {
    'esp_log.h': '#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n',
    'freertos/FreeRTOS.h': '#pragma once\n#include <cstdint>\nusing TickType_t=uint32_t;\n#define pdMS_TO_TICKS(x) ((x)/10)\n',
    'freertos/task.h': '#pragma once\ninline void vTaskDelay(unsigned) {}\n',
    'activities/ActivityManager.h': '''#pragma once
#include <cstdint>
struct ActivityManager {
    uint32_t value=1;
    static ActivityManager& instance() {static ActivityManager a; return a;}
    uint32_t generation() const {return value;}
};
''',
    'boards/MyPaperS31/KeysS31.h': '''#pragma once
namespace mypaper::board::s31::keys {
inline bool cover=false;
inline bool cover_sample(bool down,int,int y,unsigned) {
    bool old=cover; cover=down && y>=1500; return old || cover;
}
inline bool cover_active() {return cover;}
}
''',
    'M5Unified.h': '''#pragma once
#include <cstdint>
inline uint32_t simulated_now=0;
namespace lgfx {
struct touch_point_t {int16_t x=0,y=0; uint16_t size=0,id=0;};
inline uint32_t millis() {return simulated_now;}
}
struct MockTouch {
    uint8_t getGesture() {return 0;}
    bool lastReadSucceeded() const {return true;}
};
struct MockDisplay {
    MockTouch t;
    unsigned raw_reads=0;
    MockTouch* touch() {return &t;}
    int width() const {return 684;}
    int getTouchRaw(lgfx::touch_point_t*,int) {++raw_reads;return 0;}
    int getTouch(lgfx::touch_point_t*,int) {++raw_reads;return 0;}
    void convertRawXY(lgfx::touch_point_t*,int) {}
};
inline struct {MockDisplay Display;} M5;
''',
}
for name, text in stubs.items():
    path = out / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)

test = r'''
#include <cassert>
#include <cstdio>
#include <deque>
#include "input/S31TouchSampler.h"
#include "input/touch_input.cpp"
namespace s31_touch {
std::deque<Sample> captured;
bool overflowed=false;
bool init() {return true;}
bool running() {return true;}
bool pop(Sample* s) {
    if(captured.empty()) return false;
    *s=captured.front(); captured.pop_front(); return true;
}
bool pending() {return !captured.empty();}
bool take_overflow() {bool result=overflowed;overflowed=false;return result;}
void discard() {captured.clear();overflowed=false;}
void wait(bool) {}
bool pause() {return true;}
void resume() {discard();}
}
void sample(uint32_t at,bool down,int x=200,int y=300,bool valid=true) {
    s31_touch::Sample s; s.at=at;s.down=down;s.x=x;s.y=y;s.valid=valid;
    s31_touch::captured.push_back(s);
}
TouchEvent expect(TouchEventType type,uint32_t duration) {
    TouchEvent e;assert(touch_input_pop(&e));assert(e.type==type);
    assert(e.duration_ms==duration);assert(!touch_input_pop(&e));return e;
}
void none() {TouchEvent e;assert(!touch_input_pop(&e));}
void reset() {touch_input_init();simulated_now=1000;}
void release(uint32_t at=1000) {sample(at,false);touch_input_update();}
int main() {
    // The whole gesture occurs during a simulated UI stall. Duration must use
    // capture time, not replay time. This includes the old 350..600ms dead zone.
    for(unsigned ms:{30u,349u,350u,450u,599u}) {
        reset();sample(100,true);sample(100+ms,false);touch_input_update();
        expect(TouchEventType::Tap,ms);
    }
    reset();sample(100,true);sample(700,false);touch_input_update();
    expect(TouchEventType::LongPress,600);
    reset();sample(100,true);sample(700,true);sample(750,true);sample(800,false);
    touch_input_update();expect(TouchEventType::LongPress,600);
    // Fast taps collected while rendering remain two separate taps.
    reset();sample(100,true);sample(150,false);sample(200,true);sample(250,false);
    touch_input_update();TouchEvent e;
    assert(touch_input_pop(&e) && e.type==TouchEventType::Tap && e.duration_ms==50);
    assert(touch_input_pop(&e) && e.type==TouchEventType::Tap && e.duration_ms==50);none();
    reset();sample(100,true);sample(250,true,330);sample(300,false);
    touch_input_update();expect(TouchEventType::SwipeRight,150);
    // Slider ownership suppresses gestures without losing the contact snapshot.
    reset();sample(100,true);touch_input_update();assert(touch_input_contact().down);
    touch_input_claim_contact();sample(250,true,330);sample(800,false);
    touch_input_update();none();assert(!touch_input_contact().down);
    // A touch held across a page change cannot click the new page. A fresh
    // click captured while the new page renders must survive that render.
    reset();sample(100,true);touch_input_update();
    auto& am=ActivityManager::instance();touch_input_set_context(++am.value);
    sample(200,true);sample(300,false);touch_input_update();none();
    sample(400,true);sample(480,false);touch_input_update();expect(TouchEventType::Tap,80);
    reset();touch_input_set_context(++am.value);sample(100,false);
    sample(200,true);sample(300,false);touch_input_update();expect(TouchEventType::Tap,100);
    // I2C errors, overflow and stale data cancel rather than fabricate release.
    reset();sample(100,true);touch_input_update();sample(200,false,0,0,false);
    touch_input_update();none();release();none();
    sample(1100,true);sample(1200,false);simulated_now=1300;
    touch_input_update();expect(TouchEventType::Tap,100);
    reset();sample(100,true);touch_input_update();s31_touch::overflowed=true;
    sample(200,false);touch_input_update();none();release();none();
    reset();simulated_now=3000;sample(100,true);sample(200,false);
    touch_input_update();none();release(3000);none();
    sample(3100,true);sample(3200,false);simulated_now=3300;
    touch_input_update();expect(TouchEventType::Tap,100);
    // Wake/cancel swallows a held contact; cover contacts never become taps.
    reset();touch_input_resume_after_sleep();sample(100,true);sample(200,false);
    touch_input_update();none();
    reset();sample(100,true,200,1600);sample(200,false);touch_input_update();none();
    // Unsigned timestamp arithmetic also works across millis() wrap.
    reset();simulated_now=0x40;sample(0xffffffe0,true);sample(0x20,false);
    touch_input_update();expect(TouchEventType::Tap,64);
    assert(M5.Display.raw_reads==0); // UI consumer never re-reads the controller.
    puts("S31 touch: delayed replay, taps/hold/swipe, page changes, errors, overflow, wake, cover, wrap PASS");
}
'''
(out / 'recognizer.cpp').write_text(test)

# Compile the actual S31 decoder body against an injected I2C frame source.
driver = (root / 'components/M5GFX/src/lgfx/v1/touch/Touch_FT5x06.cpp').read_text()
body = driver.split('uint_fast8_t Touch_FT5x06::getTouchRaw', 1)[1]
body = body.split('#if defined(CONFIG_IDF_TARGET_ESP32S31)', 1)[1].split('#else', 1)[0]
decoder = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
struct Point {int x=0,y=0,id=0,size=0;};
struct Driver {
    bool _last_read_ok=true, ok=true;
    unsigned reads=0;
    uint8_t frame[11]{};
    bool _check_init() {return true;}
    bool _read_reg(uint8_t reg,uint8_t* data,unsigned size) {
        assert(reg==2 && size==11);++reads;
        if(!ok)return false;std::memcpy(data,frame,size);return true;
    }
    uint_fast8_t decode(Point* tp,uint_fast8_t count) {
''' + body + r'''
    }
};
int main() {
    Driver d;Point p[2];
    assert(!d.decode(p,1) && d._last_read_ok && d.reads==1);
    d.frame[0]=2;d.frame[1]=0x81;d.frame[2]=0x23;d.frame[3]=0x24;d.frame[4]=0x56;
    d.frame[7]=0x82;d.frame[8]=0x34;d.frame[9]=0x35;d.frame[10]=0x67;
    assert(d.decode(p,1)==1 && d.reads==2);
    assert(p[0].x==0x123 && p[0].y==0x456 && p[0].id==2);
    assert(d.decode(p,2)==2 && p[1].x==0x234 && p[1].y==0x567);
    d.frame[1]=0x41;assert(d.decode(p,1)==1 && p[0].id==3);
    d.frame[0]=3;assert(!d.decode(p,1) && !d._last_read_ok);
    d.ok=false;assert(!d.decode(p,1) && !d._last_read_ok);
    puts("S31 FT6336: one frame read, two contacts, UP filtering, malformed frame and I2C failure PASS");
}
'''
(out / 'decoder.cpp').write_text(decoder)
(out / 'p4.cpp').write_text(r'''
#include <cassert>
#include <cstdio>
#include "input/touch_input.cpp"
int main() {
    lgfx::touch_point_t p; p.x=200;p.y=300;
    TouchEvent e;
    touch_input_init();process_sample(p,1,100);process_sample(p,0,200);
    assert(touch_input_pop(&e) && e.type==TouchEventType::Tap && e.duration_ms==100);
    assert(!touch_input_pop(&e));
    process_sample(p,1,300);process_sample(p,0,700);
    assert(!touch_input_pop(&e)); // P4 retains its existing 350ms tap limit.
    touch_input_update();assert(M5.Display.raw_reads==1);
    puts("P4: polling path still compiles and retains tap recognition PASS");
}
''')
for name in ('recognizer', 'decoder', 'p4'):
    target = '0' if name == 'p4' else '1'
    subprocess.run(['c++', '-std=c++17', '-DCONFIG_IDF_TARGET_ESP32S31=' + target,
                    '-I', str(out), '-I', str(root / 'main'),
                    str(out / f'{name}.cpp'), '-o', str(out / name)], check=True)
    subprocess.run([str(out / name)], check=True)
