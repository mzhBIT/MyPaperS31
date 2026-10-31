#pragma once
#include "GameAudioPcm.h"
#include "S31Hardware.h"
#include <atomic>
#include <driver/i2s_std.h>
#include <freertos/task.h>

namespace emulator {
class GameAudioOutput {
  public:
    bool begin(S31Hardware &hardware);
    bool play();
    bool pause();
    void shutdown();
    void submit(const int16_t *stereo, size_t frames);
    void volume(int value) {
        volume_.store(std::max(0, std::min(100, value)));
    }
    int volume() const {
        return volume_.load();
    }
    void mute(bool value) {
        muted_.store(value);
    }
    bool muted() const {
        return muted_.load();
    }
    bool ready() const {
        return ready_.load();
    }
    bool failed() const {
        return failed_.load();
    }

  private:
    static void entry(void *context);
    void run();
    bool boot_codec();
    bool sleep(uint32_t ms);
    bool command(const char *command, uint32_t wait_ms);
    bool configure_i2s();
    uint32_t measure_rate();
    bool silence_dma();
    bool write(const void *bytes, size_t length);
    bool pa(bool enabled);
    void cleanup();
    S31Hardware *hardware_ = nullptr;
    i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
    bool tx_enabled_ = false, rx_enabled_ = false, uart_ = false, pa_on_ = false;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t queue_lock_ = nullptr, idle_ = nullptr, exited_ = nullptr;
    std::atomic<bool> stopping_{false}, playing_{false}, ready_{false}, failed_{false},
        muted_{false};
    std::atomic<int> volume_{30};
    std::atomic<uint32_t> rate_{16000}, dropped_{0};
    GameAudioQueue queue_;
    GameAudioResampler resampler_;
    int16_t *ring_ = nullptr;
    int32_t *slots_ = nullptr;
    uint8_t *zeros_ = nullptr;
    int16_t converted_[810]{};
    int16_t chunk_[128]{};
    uint32_t resampler_rate_ = 16000;
};
} // namespace emulator
