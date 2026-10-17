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

// Read-only main-thread contact snapshot. This does not enqueue press/move
// events or change the existing tap-on-release and long-press recognizer.
struct TouchContact {
    bool down = false;
    bool gesture_claimed = false;
    uint32_t sequence = 0;
    int start_x = 0, start_y = 0;
    int x = 0, y = 0;
    int max_move = 0;
};
TouchContact touch_input_contact();

void touch_input_init();
void touch_input_update();
bool touch_input_pop(TouchEvent *event);
// Main-thread only: poll more promptly while tracking a gesture or dispatching
// queued events. This does not change tap-on-release / swipe recognition.
bool touch_input_needs_fast_poll();

void touch_input_set_back_swipe_edge(BackSwipeEdge edge);
BackSwipeEdge touch_input_get_back_swipe_edge();
