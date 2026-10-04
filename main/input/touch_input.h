#pragma once

#include <cstdint>

enum class TouchEventType {
    None = 0,
    Tap,
    LongPress,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Back,
    SwipeUpLeft,
    SwipeUpRight,
    SwipeDownLeft,
    SwipeDownRight,
};

enum class BackSwipeEdge {
    LeftEdgeRightSwipe = 0,
    RightEdgeLeftSwipe,
};

struct TouchEvent {
    TouchEventType type = TouchEventType::None;
    int x = 0;
    int y = 0;
    int start_x = 0;
    int start_y = 0;
    int dx = 0;
    int dy = 0;
    uint32_t duration_ms = 0;
};

void touch_input_init();
void touch_input_update();
bool touch_input_pop(TouchEvent *event);

void touch_input_set_back_swipe_edge(BackSwipeEdge edge);
BackSwipeEdge touch_input_get_back_swipe_edge();
