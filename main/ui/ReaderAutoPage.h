#pragma once

#include <cstdint>

// Monotonic, foreground-only timer. No task, wall clock, or periodic storage.
class ReaderAutoPage {
public:
    void reset() { _started = -1; }
    bool due(int64_t now_us, int seconds, bool allowed)
    {
        if (!allowed || seconds <= 0) { reset(); return false; }
        if (_seconds != seconds || _started < 0) {
            _seconds = seconds;
            _started = now_us;
            return false;
        }
        if (now_us - _started < int64_t(seconds) * 1000000) return false;
        _started = now_us;
        return true;
    }
private:
    int64_t _started = -1;
    int _seconds = 0;
};
