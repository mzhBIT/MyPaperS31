#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace emulator {
// Continuous rational resampling. A block boundary never resets phase/history.
class GameAudioResampler {
  public:
    static constexpr uint32_t SourceRate = 32768;
    void reset(uint32_t rate) {
        rate_ = rate;
        phase_ = 0;
        previous_ = 0;
        started_ = false;
    }
    size_t convert(const int16_t *stereo, size_t frames, int16_t *output, size_t capacity) {
        if (!rate_ || rate_ > 48000 || !stereo || !output)
            return 0;
        const size_t needed = (uint64_t(phase_) + uint64_t(frames) * rate_) / SourceRate;
        if (needed > capacity)
            return 0;
        size_t count = 0;
        for (size_t i = 0; i < frames; ++i) {
            const int32_t current = (int32_t(stereo[2 * i]) + stereo[2 * i + 1]) / 2;
            if (!started_) {
                previous_ = current;
                started_ = true;
            }
            uint32_t at = SourceRate - phase_;
            while (at <= rate_) {
                output[count++] = int16_t(previous_ + int64_t(current - previous_) * at / rate_);
                at += SourceRate;
            }
            phase_ = (phase_ + rate_) % SourceRate;
            previous_ = current;
        }
        return count;
    }

  private:
    uint32_t rate_ = 16000, phase_ = 0;
    int32_t previous_ = 0;
    bool started_ = false;
};
inline int32_t game_audio_slot(int16_t mono, int volume) {
    volume = std::max(0, std::min(100, volume));
    const int64_t factor = int64_t(volume) * volume * 65536 / 10000;
    const int64_t sample = int64_t(mono) * factor;
    return int32_t(std::max(int64_t(INT32_MIN), std::min(int64_t(INT32_MAX), sample)));
}
// The caller serializes short copies. I2S waits never occur under this lock.
class GameAudioQueue {
  public:
    static constexpr size_t Capacity = 4096;
    void bind(int16_t *storage) {
        data_ = storage;
        clear();
    }
    void clear() {
        read_ = write_ = count_ = 0;
    }
    size_t count() const {
        return count_;
    }
    bool push(const int16_t *data, size_t count) {
        if (!data_ || count > Capacity - count_)
            return false;
        for (size_t i = 0; i < count; ++i) {
            data_[write_] = data[i];
            write_ = (write_ + 1) % Capacity;
        }
        count_ += count;
        return true;
    }
    size_t pop(int16_t *out, size_t max) {
        const size_t n = std::min(count_, max);
        for (size_t i = 0; i < n; ++i) {
            out[i] = data_[read_];
            read_ = (read_ + 1) % Capacity;
        }
        count_ -= n;
        return n;
    }

  private:
    int16_t *data_ = nullptr;
    size_t read_ = 0, write_ = 0, count_ = 0;
};
} // namespace emulator
