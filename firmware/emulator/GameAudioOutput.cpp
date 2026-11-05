#include "GameAudioOutput.h"
#include "boards/MyPaperS31/Pins.h"
#include <sdkconfig.h>
#if !CONFIG_IDF_TARGET_ESP32S31
#error "Emulator audio supports S31 only"
#endif
#include <cstring>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <nvs.h>

namespace emulator {
namespace pins = mypaper::board::s31;
namespace {
constexpr char TAG[] = "EMU_AUDIO";
constexpr unsigned DmaDescriptors = 6, DmaFrames = 128, ChunkFrames = 128;
constexpr size_t DmaBytes = DmaDescriptors * DmaFrames * 2 * sizeof(int32_t);
constexpr size_t PrerollFrames = 512;
bool driven(int pin) {
    auto sample = [&](bool up) {
        gpio_config_t cfg{};
        cfg.pin_bit_mask = 1ULL << pin;
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
        gpio_config(&cfg);
        esp_rom_delay_us(200);
        unsigned high = 0;
        for (unsigned i = 0; i < 64; ++i) {
            high += gpio_get_level(gpio_num_t(pin)) != 0;
            esp_rom_delay_us(10);
        }
        return high;
    };
    const unsigned down = sample(false), up = sample(true);
    gpio_set_pull_mode(gpio_num_t(pin), GPIO_FLOATING);
    const bool active = down > 0 || up < 64;
    ESP_LOGI(TAG, "clock GPIO%d %s pull-down=%u/64 pull-up=%u/64", pin,
             active ? "driven" : "high-Z", down, up);
    return active;
}
uint32_t nominal(uint32_t measured) {
    uint32_t best = 0, delta = UINT32_MAX;
    for (uint32_t r : {8000u, 11025u, 12000u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u}) {
        const uint32_t d = r > measured ? r - measured : measured - r;
        if (d < delta) {
            delta = d;
            best = r;
        }
    }
    return best && delta <= best / 20 ? best : 0;
}
} // namespace
bool GameAudioOutput::begin(S31Hardware &hardware) {
    hardware_ = &hardware;
    queue_lock_ = xSemaphoreCreateMutex();
    idle_ = xSemaphoreCreateBinary();
    exited_ = xSemaphoreCreateBinary();
    ring_ = static_cast<int16_t *>(heap_caps_calloc(GameAudioQueue::Capacity, sizeof(int16_t),
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    slots_ = static_cast<int32_t *>(
        heap_caps_malloc(ChunkFrames * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    zeros_ =
        static_cast<uint8_t *>(heap_caps_calloc(1, DmaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!queue_lock_ || !idle_ || !exited_ || !ring_ || !slots_ || !zeros_) {
        failed_.store(true);
        shutdown();
        return false;
    }
    queue_.bind(ring_);
    nvs_handle_t nvs;
    if (nvs_open("audio_out", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t saved = 30;
        if (nvs_get_u8(nvs, "spk_volume", &saved) == ESP_OK)
            volume(saved);
        nvs_close(nvs);
    }
    if (xTaskCreatePinnedToCore(entry, "emu_audio", 6144, this, 4, &task_, 0) != pdPASS) {
        task_ = nullptr;
        failed_.store(true);
        shutdown();
        return false;
    }
    return true;
}
bool GameAudioOutput::sleep(uint32_t ms) {
    while (ms && !stopping_.load()) {
        const uint32_t step = std::min<uint32_t>(ms, 10);
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    return !stopping_.load();
}
bool GameAudioOutput::command(const char *text, uint32_t wait_ms) {
    if (stopping_.load())
        return false;
    uart_flush_input(UART_NUM_1);
    const size_t length = strlen(text);
    if (uart_write_bytes(UART_NUM_1, text, length) != int(length))
        return false;
    uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "BT TX: %s", text);
    if (!sleep(wait_ms))
        return false;
    char reply[128]{};
    const int got = uart_read_bytes(UART_NUM_1, reply, sizeof(reply) - 1, pdMS_TO_TICKS(150));
    if (got <= 0) {
        ESP_LOGW(TAG, "BT command has no reply");
        return false;
    }
    for (int i = 0; i < got; ++i)
        if (reply[i] < 32 || reply[i] > 126)
            reply[i] = ' ';
    ESP_LOGI(TAG, "BT RX: %s", reply);
    return true;
}
bool GameAudioOutput::boot_codec() {
    if (!hardware_->amplifier(false) || !hardware_->enable_audio_power())
        return false;
    uart_config_t config{};
    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(UART_NUM_1, 1024, 0, 0, nullptr, 0) != ESP_OK)
        return false;
    uart_ = true;
    if (uart_param_config(UART_NUM_1, &config) != ESP_OK ||
        uart_set_pin(UART_NUM_1, pins::bt_tx, pins::bt_rx, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK)
        return false;
    if (!sleep(1200))
        return false;
    if (!command("AT+RX=2\r\n", 700) && !stopping_.load())
        (void)command("AT+RX=2\r\n", 700);
    if (stopping_.load())
        return false;
    // Some modules omit RX=2 acknowledgements; clock verification below is
    // authoritative. This is the same command sequence as the working player.
    (void)command("AT+MODE=1\r\n", 500);
    return !stopping_.load() && configure_i2s();
}
bool GameAudioOutput::configure_i2s() {
    const bool bclk = driven(pins::i2s_bclk), ws = driven(pins::i2s_ws);
    const bool master = !bclk && !ws; // Never drive clocks against the BH1098G.
    i2s_chan_config_t channel =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, master ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE);
    channel.dma_desc_num = DmaDescriptors;
    channel.dma_frame_num = DmaFrames;
    channel.auto_clear_after_cb = true;
    esp_err_t error = i2s_new_channel(&channel, &tx_, &rx_);
    if (error != ESP_OK)
        return false;
    i2s_std_config_t cfg{};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    cfg.slot_cfg.ws_width = 32;
    cfg.slot_cfg.ws_pol = false;
    cfg.slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_2
    cfg.slot_cfg.left_align = true;
    cfg.slot_cfg.big_endian = false;
    cfg.slot_cfg.bit_order_lsb = false;
#endif
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = gpio_num_t(pins::i2s_bclk);
    cfg.gpio_cfg.ws = gpio_num_t(pins::i2s_ws);
    cfg.gpio_cfg.dout = gpio_num_t(pins::i2s_dout);
    cfg.gpio_cfg.din = gpio_num_t(pins::i2s_din);
    error = i2s_channel_init_std_mode(tx_, &cfg);
    if (error == ESP_OK)
        error = i2s_channel_init_std_mode(rx_, &cfg);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "I2S init: %s", esp_err_to_name(error));
        return false;
    }
    if (i2s_channel_enable(tx_) != ESP_OK)
        return false;
    tx_enabled_ = true;
    if (i2s_channel_enable(rx_) != ESP_OK)
        return false;
    rx_enabled_ = true;
    uint32_t rate = 16000;
    if (!master) {
        rate = measure_rate();
        if (!rate && sleep(300))
            rate = measure_rate();
        if (!rate) {
            ESP_LOGE(TAG, "BH1098G clock verification failed; speaker stays off");
            return false;
        }
    }
    rate_.store(rate);
    ESP_LOGI(TAG, "BH1098G PCM ready: %s %lu Hz, 32-bit stereo slots, DMA=%u ms",
             master ? "MCU master" : "MCU slave", (unsigned long)rate,
             unsigned(DmaDescriptors * DmaFrames * 1000 / rate));
    return silence_dma();
}
uint32_t GameAudioOutput::measure_rate() {
    uint8_t scratch[1024];
    uint64_t bytes = 0;
    int64_t first = 0, last = 0;
    const int64_t deadline = esp_timer_get_time() + 800000;
    while (esp_timer_get_time() < deadline && !stopping_.load()) {
        size_t n = 0;
        const auto error = i2s_channel_read(rx_, scratch, sizeof(scratch), &n, 50);
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT)
            break;
        if (n) {
            const auto now = esp_timer_get_time();
            if (!first) {
                first = now;
                continue;
            }
            bytes += n;
            last = now;
        }
    }
    const uint32_t measured =
        last - first >= 200000 ? uint32_t((bytes / 8) * 1000000ull / (last - first)) : 0;
    const uint32_t rate = nominal(measured);
    ESP_LOGI(TAG, "master rate measured=%lu Hz nominal=%lu Hz", (unsigned long)measured,
             (unsigned long)rate);
    return rate;
}
bool GameAudioOutput::silence_dma() {
    if (!tx_)
        return false;
    if (tx_enabled_) {
        if (i2s_channel_disable(tx_) != ESP_OK)
            return false;
        tx_enabled_ = false;
    }
    size_t loaded = 0;
    if (i2s_channel_preload_data(tx_, zeros_, DmaBytes, &loaded) != ESP_OK || loaded != DmaBytes)
        return false;
    if (i2s_channel_enable(tx_) != ESP_OK)
        return false;
    tx_enabled_ = true;
    return true;
}
bool GameAudioOutput::write(const void *bytes, size_t length) {
    auto *cursor = static_cast<const uint8_t *>(bytes);
    int64_t progress = esp_timer_get_time();
    while (length && !stopping_.load()) {
        size_t n = 0;
        const auto error = i2s_channel_write(tx_, cursor, length, &n, 10);
        cursor += n;
        length -= n;
        if (n)
            progress = esp_timer_get_time();
        if ((error != ESP_OK && error != ESP_ERR_TIMEOUT) ||
            esp_timer_get_time() - progress > 300000) {
            ESP_LOGE(TAG, "I2S output stopped: %s remaining=%u", esp_err_to_name(error),
                     unsigned(length));
            return false;
        }
    }
    return !stopping_.load();
}
bool GameAudioOutput::pa(bool enabled) {
    if (pa_on_ == enabled)
        return true;
    if (!hardware_->amplifier(enabled))
        return false;
    pa_on_ = enabled;
    return true;
}
void GameAudioOutput::entry(void *context) {
    static_cast<GameAudioOutput *>(context)->run();
}
void GameAudioOutput::run() {
    bool ok = boot_codec();
    bool idle_state = false, primed = false;
    uint32_t gaps = 0;
    int volume = 0;
    int64_t last_log = esp_timer_get_time();
    ready_.store(ok);
    while (ok && !stopping_.load()) {
        if (!playing_.load()) {
            if (!idle_state) {
                ok = pa(false) && silence_dma();
                xSemaphoreTake(queue_lock_, portMAX_DELAY);
                queue_.clear();
                xSemaphoreGive(queue_lock_);
                idle_state = true;
                primed = false;
                volume = 0;
            }
            xSemaphoreGive(idle_);
            if (ok)
                ok = write(zeros_, ChunkFrames * 8);
            continue;
        }
        idle_state = false;
        size_t count = 0;
        xSemaphoreTake(queue_lock_, portMAX_DELAY);
        if (primed || queue_.count() >= PrerollFrames) {
            primed = true;
            count = queue_.pop(chunk_, ChunkFrames);
        }
        xSemaphoreGive(queue_lock_);
        if (!primed) {
            ok = write(zeros_, ChunkFrames * 8);
            continue;
        }
        if (!count)
            ++gaps;
        const bool audible = !muted_.load() && volume_.load() > 0;
        if (!pa(audible)) {
            ok = false;
            break;
        }
        const int target = audible ? volume_.load() : 0;
        for (unsigned i = 0; i < ChunkFrames; ++i) {
            if ((i & 7) == 0) {
                if (volume < target)
                    ++volume;
                else if (volume > target)
                    --volume;
            }
            const int32_t sample = game_audio_slot(i < count ? chunk_[i] : 0, volume);
            slots_[i * 2] = slots_[i * 2 + 1] = sample;
        }
        ok = write(slots_, ChunkFrames * 8);
        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 5000000) {
            if (gaps || dropped_.load())
                ESP_LOGW(TAG, "audio supply: gaps=%lu dropped_blocks=%lu", (unsigned long)gaps,
                         (unsigned long)dropped_.exchange(0));
            last_log = now;
            gaps = 0;
        }
    }
    ready_.store(false);
    if (!ok && !stopping_.load()) {
        failed_.store(true);
        ESP_LOGE(TAG, "speaker unavailable; game continues silently");
    }
    cleanup();
    xSemaphoreGive(exited_);
    vTaskDelete(nullptr);
}
void GameAudioOutput::cleanup() {
    if (hardware_) {
        hardware_->amplifier(false);
        pa_on_ = false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (tx_) {
        if (tx_enabled_)
            i2s_channel_disable(tx_);
        i2s_del_channel(tx_);
        tx_ = nullptr;
        tx_enabled_ = false;
    }
    if (rx_) {
        if (rx_enabled_)
            i2s_channel_disable(rx_);
        i2s_del_channel(rx_);
        rx_ = nullptr;
        rx_enabled_ = false;
    }
    if (uart_) {
        uart_driver_delete(UART_NUM_1);
        uart_ = false;
    }
    // P14 also supplies the board's TPS/VOUT path. Cutting it here leaves the
    // main firmware unable to wake TPS after esp_restart(), or breaks the
    // emulator display if audio initialization fails. PA was muted above.
}
bool GameAudioOutput::pause() {
    playing_.store(false);
    if (ready_.load()) {
        while (xSemaphoreTake(idle_, 0) == pdTRUE) {
        }
        if (xSemaphoreTake(idle_, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "audio pause timeout; shutting down speaker");
            shutdown();
            return false;
        }
    }
    if (queue_lock_) {
        xSemaphoreTake(queue_lock_, portMAX_DELAY);
        queue_.clear();
        xSemaphoreGive(queue_lock_);
    }
    resampler_.reset(rate_.load());
    resampler_rate_ = rate_.load();
    return !failed_.load();
}
bool GameAudioOutput::play() {
    if (!pause())
        return false;
    playing_.store(true);
    return true;
}
void GameAudioOutput::submit(const int16_t *stereo, size_t frames) {
    if (!playing_.load() || !ready_.load() || failed_.load())
        return;
    const auto rate = rate_.load();
    if (resampler_rate_ != rate) {
        resampler_.reset(rate);
        resampler_rate_ = rate;
    }
    const size_t count =
        resampler_.convert(stereo, frames, converted_, sizeof(converted_) / sizeof(converted_[0]));
    if (xSemaphoreTake(queue_lock_, 0) != pdTRUE) {
        dropped_.fetch_add(1);
        return;
    }
    if (!queue_.push(converted_, count))
        dropped_.fetch_add(1);
    xSemaphoreGive(queue_lock_);
}
bool GameAudioOutput::shutdown() {
    playing_.store(false);
    stopping_.store(true);
    if (task_) {
        if (xSemaphoreTake(exited_, pdMS_TO_TICKS(2000)) != pdTRUE) {
            failed_.store(true);
            if (hardware_)
                hardware_->amplifier(false);
            // The worker may still be inside an SDK I2S teardown call. Do not
            // delete it or free its DMA/queue/semaphores underneath that call.
            ESP_LOGE(TAG, "audio shutdown timed out; retaining worker resources for restart");
            return false;
        }
        task_ = nullptr;
    } else
        cleanup();
    ready_.store(false);
    free(ring_);
    free(slots_);
    free(zeros_);
    ring_ = nullptr;
    slots_ = nullptr;
    zeros_ = nullptr;
    if (queue_lock_) {
        vSemaphoreDelete(queue_lock_);
        queue_lock_ = nullptr;
    }
    if (idle_) {
        vSemaphoreDelete(idle_);
        idle_ = nullptr;
    }
    if (exited_) {
        vSemaphoreDelete(exited_);
        exited_ = nullptr;
    }
    return true;
}
} // namespace emulator
