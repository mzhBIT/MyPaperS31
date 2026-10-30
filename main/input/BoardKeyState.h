#pragma once
#include <cstdint>

namespace board_keys {
enum class Key : uint8_t { None, Boot, VolumeUp, VolumeDown, Power, Home, Previous, Next };
enum class Gesture : uint8_t { None, Click, LongPress, Repeat };

constexpr int volume_delta(Key key, Gesture gesture) {
    const int direction = key == Key::VolumeUp ? 1 : key == Key::VolumeDown ? -1 : 0;
    return direction * (gesture == Gesture::Click ? 10 : gesture == Gesture::LongPress ? 100 : 0);
}

// Pure state machine, independent of GPIO/I2C and application callbacks.
struct Button {
    bool stable = false, candidate = false, suppressed = false, long_sent = false;
    uint32_t candidate_at = 0, down_at = 0;
    constexpr void reset(bool down, uint32_t now) {
        stable = candidate = down; suppressed = down; long_sent = false;
        candidate_at = down_at = now;
    }
    constexpr void cancel_held() { if (stable || candidate) suppressed = true; }
    constexpr Gesture sample(bool down, uint32_t now, uint32_t long_ms) {
        if (down != candidate) { candidate = down; candidate_at = now; }
        if (candidate != stable && uint32_t(now-candidate_at) >= 30) {
            stable = candidate;
            if (stable) { down_at = now; long_sent = false; }
            else {
                const bool click = !suppressed && !long_sent;
                suppressed = long_sent = false;
                return click ? Gesture::Click : Gesture::None;
            }
        }
        if (stable && !suppressed && !long_sent && uint32_t(now-down_at) >= long_ms) {
            long_sent = true;
            return Gesture::LongPress;
        }
        return Gesture::None;
    }
};

constexpr Key cover_hit(int x, int y) {
    if (y < 1570 || y > 1630) return Key::None;
    return x >= 70 && x <= 130 ? Key::Home
         : x >= 170 && x <= 230 ? Key::Previous
         : x >= 270 && x <= 330 ? Key::Next : Key::None;
}
struct Cover {
    bool active = false, cancelled = false, long_sent = false;
    Key key = Key::None;
    uint32_t down_at = 0, repeat_at = 0;
    constexpr void cancel() { if (active) cancelled = true; }
    constexpr Gesture sample(bool down, Key hit, uint32_t now, bool allow_repeat) {
        if (!active) {
            if (down) {
                active = true; cancelled = hit == Key::None; key = hit;
                long_sent = false; down_at = repeat_at = now;
            }
            return Gesture::None;
        }
        if (!down) {
            active = false;
            return !cancelled && !long_sent ? Gesture::Click : Gesture::None;
        }
        // Sliding between keys or from bezel to screen cancels the contact.
        if (hit != key) cancelled = true;
        if (cancelled) return Gesture::None;
        if (!long_sent && uint32_t(now-down_at) >= 500) {
            long_sent = true; repeat_at = now;
            return Gesture::LongPress;
        }
        if (long_sent && key != Key::Home && allow_repeat && uint32_t(now-repeat_at) >= 350) {
            repeat_at = now;
            return Gesture::Repeat;
        }
        return Gesture::None;
    }
};
}
