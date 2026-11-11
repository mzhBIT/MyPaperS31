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
void touch_input_cancel_until_release();
void touch_input_update();
bool touch_input_pop(TouchEvent *event);
// Main-thread only: consume samples promptly while tracking a gesture or
// dispatching queued events. S31 hardware sampling runs independently.
bool touch_input_needs_fast_poll();
// Idle wait can be interrupted by a freshly captured S31 touch sample.
void touch_input_wait(bool fast_poll);
bool touch_input_pause_for_sleep();
void touch_input_resume_after_sleep();
// Called before rendering a new activity, so samples during rendering belong to it.
void touch_input_set_context(uint32_t generation);

void touch_input_set_back_swipe_edge(BackSwipeEdge edge);
BackSwipeEdge touch_input_get_back_swipe_edge();

// Capture the current contact for a slider; retain coordinates but suppress gestures until release.
void touch_input_claim_contact();
