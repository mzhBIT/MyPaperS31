#include "ui/ReaderSwipe.h"
#include "ui/ReaderTap.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main() {
    const TouchEventType types[]={TouchEventType::SwipeLeft,TouchEventType::SwipeRight,
        TouchEventType::SwipeUp,TouchEventType::SwipeDown,TouchEventType::SwipeUpLeft,
        TouchEventType::SwipeDownRight,TouchEventType::SwipeUpRight,TouchEventType::SwipeDownLeft};
    const int dx[]={-100,100,0,0,-100,100,100,-100};
    const int dy[]={0,0,-100,100,-100,100,-100,100};
    const int animation[]={1,-1,2,-2,3,-3,4,-4};
    const int page[]={1,-1,1,-1,1,-1,-1,1};
    for (int i=0;i<8;++i) {
        TouchEvent e; e.type=types[i]; e.dx=dx[i]; e.dy=dy[i];
        assert(reader_swipe_animation(e)==animation[i]);
        assert(reader_swipe_page_direction(e)==page[i]);
        // Hardware gesture with no movement coordinates uses its type.
        e.dx=e.dy=0;
        assert(reader_swipe_animation(e)==animation[i]);
    }
    TouchEvent e; e.type=TouchEventType::SwipeLeft; e.dx=-100;
    e.dy=41; assert(reader_swipe_animation(e)==1);
    e.dy=42; assert(reader_swipe_animation(e)==-4);
    e.type=TouchEventType::Tap;
    assert(reader_swipe_page_direction(e)==0);
    e.type=TouchEventType::Back;
    assert(reader_swipe_page_direction(e)==0);
    auto tap=[](int x,int y,int page,int animation) {
        const auto action=reader_tap_action(x,y,684,1216);
        assert(action.page==page && action.animation==animation);
    };
    tap(100,200,-1,-3); tap(342,100,-1,-2); tap(584,200,1,-4);
    tap(100,600,-1,-1); tap(342,600,0,0); tap(584,600,1,1);
    tap(100,1000,-1,4); tap(342,1100,1,2); tap(584,1000,1,3);
    // Column borders and independent side/middle row borders are half-open.
    tap(199,300,-1,-3); tap(200,300,0,0);
    tap(483,900,0,0); tap(484,900,1,3);
    tap(342,249,-1,-2); tap(342,250,0,0);
    tap(342,965,0,0); tap(342,966,1,2);
    tap(100,404,-1,-3); tap(100,405,-1,-1);
    tap(100,810,-1,-1); tap(100,811,-1,4);
    assert(reader_tap_action(342,600,684,1216).page==0);
    for (int w : {320,540,684,1216}) for (int h : {240,960,1216}) {
        for (int y=0;y<h;++y) for (int x=0;x<w;x+=19) {
            const auto a=reader_tap_action(x,y,w,h);
            assert(a.page>=-1 && a.page<=1);
            assert(!a.page ? a.animation==0 : a.animation!=0 && a.animation>=-4 && a.animation<=4);
        }
    }
    std::puts("Reader gestures: eight swipes, nine tap regions, independent borders and screen sizes passed");
}
