#include "input/BoardKeyState.h"
using namespace board_keys;
constexpr bool physical_click() {
    Button b; b.reset(false,0);
    return b.sample(true,10,500)==Gesture::None
        && b.sample(true,39,500)==Gesture::None
        && b.sample(true,40,500)==Gesture::None
        && b.sample(false,100,500)==Gesture::None
        && b.sample(false,130,500)==Gesture::Click;
}
constexpr bool physical_long(unsigned threshold) {
    Button b; b.reset(false,0);
    b.sample(true,10,threshold); b.sample(true,40,threshold);
    return b.sample(true,39+threshold,threshold)==Gesture::None
        && b.sample(true,40+threshold,threshold)==Gesture::LongPress
        && b.sample(true,41+threshold,threshold)==Gesture::None
        && b.sample(false,50+threshold,threshold)==Gesture::None
        && b.sample(false,80+threshold,threshold)==Gesture::None;
}
constexpr bool cancel_held() {
    Button b; b.reset(true,0);
    if (b.sample(true,1000,500)!=Gesture::None) return false;
    b.sample(false,1010,500);
    if (b.sample(false,1040,500)!=Gesture::None) return false;
    b.sample(true,1050,500); b.sample(true,1080,500); b.cancel_held();
    b.sample(false,1100,500);
    return b.sample(false,1130,500)==Gesture::None;
}
constexpr bool cover_click_long_repeat() {
    Cover c;
    if (c.sample(true,Key::Home,0,true)!=Gesture::None) return false;
    if (c.sample(false,Key::None,100,true)!=Gesture::Click) return false;
    c.sample(true,Key::Previous,200,true);
    if (c.sample(true,Key::Previous,700,true)!=Gesture::LongPress) return false;
    if (c.sample(true,Key::Previous,1050,false)!=Gesture::None) return false;
    if (c.sample(true,Key::Previous,1051,true)!=Gesture::Repeat) return false;
    return c.sample(false,Key::None,1060,true)==Gesture::None;
}
constexpr bool cover_cancel() {
    Cover c; c.sample(true,Key::Previous,0,true);
    c.sample(true,Key::Next,50,true);
    if(c.sample(false,Key::None,100,true)!=Gesture::None) return false;
    c.sample(true,Key::Home,200,true); c.cancel();
    return c.sample(true,Key::Home,800,true)==Gesture::None
        && c.sample(false,Key::None,900,true)==Gesture::None;
}
constexpr bool wrap_clock() {
    Button b; b.reset(false,0xfffffff0u);
    b.sample(true,0xfffffff1u,500); b.sample(true,0x20u,500);
    return b.sample(true,0x214u,500)==Gesture::LongPress;
}
static_assert(physical_click());
static_assert(physical_long(500) && physical_long(3000));
static_assert(cancel_held() && cover_click_long_repeat() && cover_cancel() && wrap_clock());
static_assert(cover_hit(100,1600)==Key::Home);
static_assert(cover_hit(200,1600)==Key::Previous);
static_assert(cover_hit(300,1600)==Key::Next);
static_assert(cover_hit(100,1215)==Key::None && cover_hit(150,1600)==Key::None);
static_assert(volume_delta(Key::VolumeUp,Gesture::Click)==10);
static_assert(volume_delta(Key::VolumeDown,Gesture::Click)==-10);
static_assert(volume_delta(Key::VolumeUp,Gesture::LongPress)==100);
static_assert(volume_delta(Key::VolumeDown,Gesture::LongPress)==-100);
static_assert(volume_delta(Key::VolumeUp,Gesture::None)==0);
static_assert(volume_delta(Key::VolumeDown,Gesture::Repeat)==0);
static_assert(volume_delta(Key::Power,Gesture::Click)==0);
int main() {}
