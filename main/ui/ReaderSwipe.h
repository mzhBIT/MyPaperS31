#pragma once
#include "input/touch_input.h"

// Signed codes preserve +1 right-to-left / -1 left-to-right animation.
inline int reader_swipe_page_direction(const TouchEvent &event) {
    switch (event.type) {
    case TouchEventType::SwipeLeft: case TouchEventType::SwipeUp:
    case TouchEventType::SwipeUpLeft: case TouchEventType::SwipeDownLeft: return 1;
    case TouchEventType::SwipeRight: case TouchEventType::SwipeDown:
    case TouchEventType::SwipeUpRight: case TouchEventType::SwipeDownRight: return -1;
    default: return 0;
    }
}
inline int reader_swipe_animation(const TouchEvent &event) {
    const int x=event.dx, y=event.dy;
    const int ax=x<0?-x:x, ay=y<0?-y:y;
    if (ax || ay) {
        if (ay*1000 < ax*414) return x<0?1:-1;
        if (ax*1000 < ay*414) return y<0?2:-2;
        if (x<0) return y<0?3:-4;
        return y<0?4:-3;
    }
    switch (event.type) {
    case TouchEventType::SwipeUp: return 2;
    case TouchEventType::SwipeDown: return -2;
    case TouchEventType::SwipeUpLeft: return 3;
    case TouchEventType::SwipeDownRight: return -3;
    case TouchEventType::SwipeUpRight: return 4;
    case TouchEventType::SwipeDownLeft: return -4;
    default: return reader_swipe_page_direction(event);
    }
}
