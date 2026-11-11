#include "input/touch_input.h"

#include <M5Unified.h>
#include <esp_log.h>
#include <algorithm>
#include <cstdlib>
#if CONFIG_IDF_TARGET_ESP32S31
#include "boards/MyPaperS31/KeysS31.h"
#include "activities/ActivityManager.h"
#include "input/S31TouchSampler.h"
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr int TAP_MAX_MOVE_PX = 20;
#if CONFIG_IDF_TARGET_ESP32S31
constexpr uint32_t TAP_MAX_DURATION_MS = 599;
#else
constexpr uint32_t TAP_MAX_DURATION_MS = 350;
#endif
constexpr int LONG_PRESS_MAX_MOVE_PX = 25;
constexpr uint32_t LONG_PRESS_MS = 600;
constexpr int SWIPE_MIN_DISTANCE_PX = 60;
constexpr int SWIPE_EARLY_DISTANCE_PX = 90;
constexpr int SWIPE_MAX_OFF_AXIS_PX = 70;
constexpr int SWIPE_EARLY_MAX_OFF_AXIS_PX = 80;
constexpr uint32_t SWIPE_MIN_DURATION_MS = 40;
constexpr uint32_t SWIPE_EARLY_MAX_DURATION_MS = 1200;
constexpr uint32_t SWIPE_RELEASE_MAX_DURATION_MS = 1800;
constexpr int SWIPE_EARLY_MIN_VELOCITY_PX_PER_SEC = 260;
constexpr int SWIPE_RELEASE_MIN_VELOCITY_PX_PER_SEC = 120;
constexpr int BACK_EDGE_WIDTH_PX = 40;
constexpr int BACK_MIN_DISTANCE_PX = 80;
constexpr size_t EVENT_QUEUE_SIZE = 8;

const char *TAG = "TOUCH_INPUT";

TouchEvent s_events[EVENT_QUEUE_SIZE];
size_t s_head = 0;
size_t s_tail = 0;
BackSwipeEdge s_back_edge = BackSwipeEdge::LeftEdgeRightSwipe;

bool s_pressed = false;
bool s_suppress_until_release = false;
bool s_long_press_sent = false;
bool s_swipe_sent = false;
bool s_contact_claimed = false;
int s_start_x = 0;
int s_start_y = 0;
int s_last_x = 0;
int s_last_y = 0;
int s_peak_x = 0;
int s_peak_y = 0;
uint32_t s_start_ms = 0;
uint8_t s_last_hw_gesture = 0;
uint32_t s_last_hw_debug_ms = 0;
uint32_t s_contact_sequence = 0;
#if CONFIG_IDF_TARGET_ESP32S31
uint32_t s_context = 0;
#endif

void push_event(const TouchEvent &event)
{
    size_t next = (s_tail + 1) % EVENT_QUEUE_SIZE;
    if (next == s_head) {
        s_head = (s_head + 1) % EVENT_QUEUE_SIZE;
    }
    s_events[s_tail] = event;
    s_tail = next;
}

int abs_i(int value)
{
    return value < 0 ? -value : value;
}

void update_peak_point(int x, int y)
{
    int current_dist = abs_i(x - s_start_x) + abs_i(y - s_start_y);
    int peak_dist = abs_i(s_peak_x - s_start_x) + abs_i(s_peak_y - s_start_y);
    if (current_dist > peak_dist) {
        s_peak_x = x;
        s_peak_y = y;
    }
}

TouchEvent make_event(TouchEventType type, int x, int y, uint32_t now)
{
    TouchEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.start_x = s_start_x;
    event.start_y = s_start_y;
    event.dx = x - s_start_x;
    event.dy = y - s_start_y;
    event.duration_ms = now - s_start_ms;
    return event;
}

TouchEvent make_motion_event(TouchEventType type, int x, int y, int dx, int dy, uint32_t now)
{
    TouchEvent event = make_event(type, x, y, now);
    event.dx = dx;
    event.dy = dy;
    return event;
}

bool is_back_swipe(const TouchEvent &event)
{
    int screen_w = M5.Display.width();
    if (s_back_edge == BackSwipeEdge::LeftEdgeRightSwipe) {
        return event.start_x <= BACK_EDGE_WIDTH_PX &&
               event.dx >= BACK_MIN_DISTANCE_PX &&
               abs_i(event.dy) <= SWIPE_MAX_OFF_AXIS_PX;
    }
    return event.start_x >= screen_w - BACK_EDGE_WIDTH_PX &&
           event.dx <= -BACK_MIN_DISTANCE_PX &&
           abs_i(event.dy) <= SWIPE_MAX_OFF_AXIS_PX;
}

TouchEventType classify_swipe(int dx, int dy)
{
    int adx = abs_i(dx);
    int ady = abs_i(dy);
    if (adx > SWIPE_MAX_OFF_AXIS_PX && ady > SWIPE_MAX_OFF_AXIS_PX &&
        std::min(adx,ady)*1000 >= std::max(adx,ady)*414) {
        return dy<0 ? (dx<0 ? TouchEventType::SwipeUpLeft : TouchEventType::SwipeUpRight)
                    : (dx<0 ? TouchEventType::SwipeDownLeft : TouchEventType::SwipeDownRight);
    }
    if (adx >= SWIPE_MIN_DISTANCE_PX && ady <= SWIPE_MAX_OFF_AXIS_PX) {
        return dx < 0 ? TouchEventType::SwipeLeft : TouchEventType::SwipeRight;
    }
    if (ady >= SWIPE_MIN_DISTANCE_PX && adx <= SWIPE_MAX_OFF_AXIS_PX) {
        return dy < 0 ? TouchEventType::SwipeUp : TouchEventType::SwipeDown;
    }
    return TouchEventType::None;
}

TouchEventType classify_swipe_with_limits(int dx, int dy, int min_distance, int max_off_axis)
{
    int adx = abs_i(dx);
    int ady = abs_i(dy);
    if (adx >= min_distance && ady >= min_distance && adx>max_off_axis && ady>max_off_axis &&
        std::min(adx,ady)*1000 >= std::max(adx,ady)*414) {
        return dy<0 ? (dx<0 ? TouchEventType::SwipeUpLeft : TouchEventType::SwipeUpRight)
                    : (dx<0 ? TouchEventType::SwipeDownLeft : TouchEventType::SwipeDownRight);
    }
    if (adx >= min_distance && ady <= max_off_axis) {
        return dx < 0 ? TouchEventType::SwipeLeft : TouchEventType::SwipeRight;
    }
    if (ady >= min_distance && adx <= max_off_axis) {
        return dy < 0 ? TouchEventType::SwipeUp : TouchEventType::SwipeDown;
    }
    return TouchEventType::None;
}

bool has_swipe_velocity(int dx, int dy, uint32_t duration_ms, uint32_t max_duration_ms, int min_velocity_px_per_sec)
{
    if (duration_ms < SWIPE_MIN_DURATION_MS || duration_ms > max_duration_ms) return false;
    int distance = std::max(abs_i(dx), abs_i(dy));
    return (distance * 1000) / (int)duration_ms >= min_velocity_px_per_sec;
}

#if !CONFIG_IDF_TARGET_ESP32S31
TouchEventType classify_gt911_gesture(uint8_t gesture)
{
    switch (gesture) {
    case 0x18:
        return TouchEventType::SwipeLeft;
    case 0x1C:
        return TouchEventType::SwipeRight;
    case 0x10:
        return TouchEventType::SwipeUp;
    case 0x14:
        return TouchEventType::SwipeDown;
    default:
        return TouchEventType::None;
    }
}

bool push_hardware_gesture(uint32_t now, bool pressed, const lgfx::touch_point_t &point)
{
    auto *touch = M5.Display.touch();
    if (!touch) return false;

    uint8_t gesture = touch->getGesture();
    if (pressed && now - s_last_hw_debug_ms >= 2000) {
        s_last_hw_debug_ms = now;
        ESP_LOGI(TAG, "gt911 gesture raw=0x%02X pressed=%d x=%d y=%d", gesture, pressed ? 1 : 0, point.x, point.y);
    }
    if (gesture == 0) {
        s_last_hw_gesture = 0;
        return false;
    }
    if (gesture == s_last_hw_gesture) return false;
    s_last_hw_gesture = gesture;

    TouchEventType type = classify_gt911_gesture(gesture);
    ESP_LOGI(TAG, "gt911 gesture=0x%02X type=%d", gesture, (int)type);
    if (type == TouchEventType::None) return false;

    if (pressed) {
        if (!s_pressed) {
            s_start_x = point.x;
            s_start_y = point.y;
            s_start_ms = now;
        }
        s_last_x = point.x;
        s_last_y = point.y;
    }

    TouchEvent event;
    event.type = type;
    event.x = s_last_x;
    event.y = s_last_y;
    event.start_x = s_start_x;
    event.start_y = s_start_y;
    event.dx = s_last_x - s_start_x;
    event.dy = s_last_y - s_start_y;
    event.duration_ms = s_pressed ? now - s_start_ms : 0;

    if (is_back_swipe(event)) {
        event.type = TouchEventType::Back;
    }
    push_event(event);
    return true;
}
#endif

} // namespace

static void reset_recognizer(bool suppress)
{
    s_suppress_until_release = suppress;
    s_contact_claimed = false;
    s_head = 0;
    s_tail = 0;
    s_pressed = false;
    s_long_press_sent = false;
    s_swipe_sent = false;
    s_last_hw_gesture = 0;
    s_last_hw_debug_ms = 0;
    s_peak_x = 0;
    s_peak_y = 0;
}

void touch_input_init()
{
    reset_recognizer(false);
    s_contact_sequence = 0;
#if CONFIG_IDF_TARGET_ESP32S31
    s_context = ActivityManager::instance().generation();
    s31_touch::init();
    s31_touch::discard();
#endif
}

void touch_input_cancel_until_release()
{
    reset_recognizer(true);
#if CONFIG_IDF_TARGET_ESP32S31
    s31_touch::discard();
#endif
}

static void process_sample(lgfx::touch_point_t point, int count, uint32_t now)
{
#if CONFIG_IDF_TARGET_ESP32S31
    // Cover keys are reported as RAW Y~1600, outside the screen. Identify
    // them before affine/rotation conversion and never read FT twice.
    if (s_suppress_until_release) {
        mypaper::board::s31::keys::cover_sample(count > 0, point.x, point.y, now);
        if (!count) s_suppress_until_release = false;
        return;
    }
    if (mypaper::board::s31::keys::cover_sample(count > 0, point.x, point.y, now)) {
        s_head = s_tail; // Bezel contacts must not replay a queued screen tap.
        s_pressed = s_long_press_sent = s_swipe_sent = false;
        s_peak_x = s_peak_y = 0;
        return;
    }
    M5.Display.convertRawXY(&point, count);
#else
    if (s_suppress_until_release) {
        if (!count) s_suppress_until_release = false;
        return;
    }
#endif
    bool pressed = count > 0;

#if !CONFIG_IDF_TARGET_ESP32S31
    bool hardware_gesture = !s_contact_claimed && push_hardware_gesture(now, pressed, point);
    if (hardware_gesture) {
        s_pressed = false;
        s_long_press_sent = false;
        s_swipe_sent = false;
        return;
    }
#endif

    if (pressed && !s_pressed) {
        s_contact_claimed = false;
        ++s_contact_sequence;
        s_pressed = true;
        s_long_press_sent = false;
        s_swipe_sent = false;
        s_start_x = point.x;
        s_start_y = point.y;
        s_last_x = point.x;
        s_last_y = point.y;
        s_peak_x = point.x;
        s_peak_y = point.y;
        s_start_ms = now;
        return;
    }

    if (pressed && s_pressed) {
        s_last_x = point.x;
        s_last_y = point.y;
        update_peak_point(s_last_x, s_last_y);
        if (s_contact_claimed) return;
        int dx = s_last_x - s_start_x;
        int dy = s_last_y - s_start_y;
        uint32_t duration = now - s_start_ms;
        if (!s_swipe_sent &&
            !s_long_press_sent &&
            has_swipe_velocity(dx,
                               dy,
                               duration,
                               SWIPE_EARLY_MAX_DURATION_MS,
                               SWIPE_EARLY_MIN_VELOCITY_PX_PER_SEC)) {
            TouchEventType swipe = classify_swipe_with_limits(dx,
                                                              dy,
                                                              SWIPE_EARLY_DISTANCE_PX,
                                                              SWIPE_EARLY_MAX_OFF_AXIS_PX);
            if (swipe != TouchEventType::None) {
                TouchEvent event = make_motion_event(swipe, s_last_x, s_last_y, dx, dy, now);
                if (is_back_swipe(event)) {
                    event.type = TouchEventType::Back;
                }
                push_event(event);
                s_swipe_sent = true;
                ESP_LOGD(TAG, "early swipe type=%d dx=%d dy=%d duration=%u",
                         (int)event.type,
                         event.dx,
                         event.dy,
                         (unsigned)event.duration_ms);
                return;
            }
        }
        if (!s_swipe_sent &&
            !s_long_press_sent &&
            now - s_start_ms >= LONG_PRESS_MS &&
            std::max(abs_i(s_peak_x-s_start_x), abs_i(s_peak_y-s_start_y)) <= LONG_PRESS_MAX_MOVE_PX) {
            push_event(make_event(TouchEventType::LongPress, s_last_x, s_last_y, now));
            s_long_press_sent = true;
        }
        return;
    }

    if (!pressed && s_pressed) {
        TouchEvent event = make_event(TouchEventType::None, s_last_x, s_last_y, now);
        int peak_dx = s_peak_x - s_start_x;
        int peak_dy = s_peak_y - s_start_y;
        int move = std::max(abs_i(peak_dx), abs_i(peak_dy));
        if (s_swipe_sent || s_contact_claimed || s_long_press_sent) {
            // Swipe was already emitted while the finger was still down.
        } else if (!s_long_press_sent &&
            event.duration_ms <= TAP_MAX_DURATION_MS &&
            move <= TAP_MAX_MOVE_PX) {
            event.type = TouchEventType::Tap;
            push_event(event);
#if CONFIG_IDF_TARGET_ESP32S31
        } else if (!s_long_press_sent && event.duration_ms >= LONG_PRESS_MS &&
                   move <= LONG_PRESS_MAX_MOVE_PX) {
            // Release may be the first sample crossing the long-press threshold.
            event.type = TouchEventType::LongPress;
            push_event(event);
#endif
        } else {
            event.dx = peak_dx;
            event.dy = peak_dy;
            event.x = s_peak_x;
            event.y = s_peak_y;
            event.type = TouchEventType::None;
            if (has_swipe_velocity(event.dx,
                                   event.dy,
                                   event.duration_ms,
                                   SWIPE_RELEASE_MAX_DURATION_MS,
                                   SWIPE_RELEASE_MIN_VELOCITY_PX_PER_SEC)) {
                event.type = classify_swipe(event.dx, event.dy);
            }
            if (event.type != TouchEventType::None) {
                if (is_back_swipe(event)) {
                    event.type = TouchEventType::Back;
                }
                push_event(event);
            }
        }

        s_pressed = false;
        s_long_press_sent = false;
        s_swipe_sent = false;
        s_contact_claimed = false;
        s_peak_x = 0;
        s_peak_y = 0;
    }
}

#if CONFIG_IDF_TARGET_ESP32S31
static bool sync_context()
{
    const auto generation = ActivityManager::instance().generation();
    if (s_context == generation) return false;
    touch_input_set_context(generation);
    return true;
}
#endif

void touch_input_set_context(uint32_t generation)
{
#if CONFIG_IDF_TARGET_ESP32S31
    s_context = generation;
    touch_input_cancel_until_release();
#else
    (void)generation;
#endif
}

void touch_input_update()
{
#if CONFIG_IDF_TARGET_ESP32S31
    if (sync_context()) return;
    if (s31_touch::running()) {
        if (s31_touch::take_overflow()) {
            ESP_LOGW(TAG, "sample queue overflow; cancelling contact");
            touch_input_cancel_until_release();
            return;
        }
        s31_touch::Sample sample;
        while (s31_touch::pop(&sample)) {
            if (!sample.valid || uint32_t(lgfx::millis()-sample.at) > 1500) {
                touch_input_cancel_until_release();
                return;
            }
            lgfx::touch_point_t point{};
            point.x = sample.x; point.y = sample.y;
            process_sample(point, sample.down ? 1 : 0, sample.at);
        }
        return;
    }
#endif
    lgfx::touch_point_t point{};
#if CONFIG_IDF_TARGET_ESP32S31
    const int count = M5.Display.getTouchRaw(&point, 1);
    auto* touch = M5.Display.touch();
    if (touch && !touch->lastReadSucceeded()) { reset_recognizer(true); return; }
#else
    const int count = M5.Display.getTouch(&point, 1);
#endif
    process_sample(point, count, lgfx::millis());
}

bool touch_input_pop(TouchEvent *event)
{
#if CONFIG_IDF_TARGET_ESP32S31
    if (sync_context()) return false;
    if (s31_touch::take_overflow()) {
        touch_input_cancel_until_release();
        return false;
    }
#endif
    if (!event || s_head == s_tail) return false;
    *event = s_events[s_head];
    s_head = (s_head + 1) % EVENT_QUEUE_SIZE;
    return true;
}

bool touch_input_needs_fast_poll()
{
#if CONFIG_IDF_TARGET_ESP32S31
    if (s31_touch::pending()) return true;
    if (mypaper::board::s31::keys::cover_active()) return true;
#endif
    return s_pressed || s_head != s_tail;
}

void touch_input_wait(bool fast_poll)
{
#if CONFIG_IDF_TARGET_ESP32S31
    s31_touch::wait(fast_poll);
#else
    vTaskDelay(fast_poll ? 1 : std::max<TickType_t>(1, pdMS_TO_TICKS(20)));
#endif
}

bool touch_input_pause_for_sleep()
{
#if CONFIG_IDF_TARGET_ESP32S31
    return s31_touch::pause();
#else
    return true;
#endif
}

void touch_input_resume_after_sleep()
{
    touch_input_cancel_until_release();
#if CONFIG_IDF_TARGET_ESP32S31
    s31_touch::resume();
#endif
}

TouchContact touch_input_contact()
{
    return {s_pressed, s_long_press_sent || s_swipe_sent, s_contact_sequence,
            s_start_x, s_start_y, s_last_x, s_last_y,
            std::max(abs_i(s_peak_x - s_start_x), abs_i(s_peak_y - s_start_y))};
}

void touch_input_set_back_swipe_edge(BackSwipeEdge edge)
{
    s_back_edge = edge;
}

BackSwipeEdge touch_input_get_back_swipe_edge()
{
    return s_back_edge;
}

void touch_input_claim_contact() { if (s_pressed) s_contact_claimed = true; }
