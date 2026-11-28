#pragma once

#include <cstdint>
#include <optional>

namespace pomodoro {

enum class Phase { Focus, ShortBreak, LongBreak };
enum class State { Ready, Running, Paused };

constexpr int cycle_size = 4;
constexpr int64_t second_us = 1000000;

constexpr int duration_seconds(Phase phase)
{
    switch (phase) {
        case Phase::Focus: return 25 * 60;
        case Phase::ShortBreak: return 5 * 60;
        case Phase::LongBreak: return 15 * 60;
    }
    return 0;
}

class Timer {
public:
    Phase phase() const { return _phase; }
    State state() const { return _state; }
    int rounds() const { return _rounds; }
    bool running() const { return _state == State::Running; }
    std::optional<Phase> completed() const { return _completed; }

    int remaining_seconds(int64_t now) const
    {
        const auto left = remaining(now);
        return int((left + second_us - 1) / second_us);
    }

    bool update(int64_t now)
    {
        if (!running() || now < _deadline) return false;
        _completed = _phase;
        if (_phase == Phase::Focus) {
            ++_rounds;
            _phase = _rounds == cycle_size ? Phase::LongBreak : Phase::ShortBreak;
        } else {
            if (_phase == Phase::LongBreak) _rounds = 0;
            _phase = Phase::Focus;
        }
        _state = State::Ready;
        _remaining = int64_t(duration_seconds(_phase)) * second_us;
        return true;
    }

    void toggle(int64_t now)
    {
        // A tap arriving at expiry belongs to the old interval.
        if (update(now)) return;
        if (running()) {
            _remaining = remaining(now);
            _state = State::Paused;
        } else {
            _deadline = now + _remaining;
            _completed.reset();
            _state = State::Running;
        }
    }

    void reset(int64_t now)
    {
        update(now);
        _remaining = int64_t(duration_seconds(_phase)) * second_us;
        _completed.reset();
        _state = State::Ready;
    }

private:
    Phase _phase = Phase::Focus;
    State _state = State::Ready;
    int _rounds = 0;
    int64_t _deadline = 0;
    int64_t _remaining = int64_t(duration_seconds(Phase::Focus)) * second_us;
    std::optional<Phase> _completed;

    int64_t remaining(int64_t now) const
    {
        if (!running()) return _remaining;
        return now < _deadline ? _deadline - now : 0;
    }
};

} // namespace pomodoro
