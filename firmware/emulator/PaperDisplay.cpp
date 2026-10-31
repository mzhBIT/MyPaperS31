#include "PaperDisplay.h"
#include "boards/MyPaperS31/Pins.h"
#include <sdkconfig.h>
#if !CONFIG_IDF_TARGET_ESP32S31
#error "PaperDisplay supports S31 only"
#endif
#include <cstring>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace emulator {
using namespace mypaper::board::s31;
constexpr char TAG[] = "EMU_SCAN";
constexpr size_t LineBytes = PanelWidth / 4 + 16; // Paperboy's source-clock padding.
constexpr unsigned ImageFields = 8;
bool IRAM_ATTR PaperDisplay::dma_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *,
                                      void *context) {
    static_cast<PaperDisplay *>(context)->dma_finished_.store(true, std::memory_order_release);
    return false;
}
bool PaperDisplay::init(S31Hardware &hardware) {
    hardware_ = &hardware;
    // Explicit output mode clears boot-time open-drain configuration on S31.
    for (int pin : epd_data) {
        gpio_set_direction(gpio_num_t(pin), GPIO_MODE_OUTPUT);
        gpio_set_level(gpio_num_t(pin), 0);
    }
    for (int pin : {epd_xcl, epd_xle, epd_xoe, epd_xstl, epd_mode, epd_ckv, epd_spv, epd_border}) {
        gpio_set_direction(gpio_num_t(pin), GPIO_MODE_OUTPUT);
        gpio_set_level(gpio_num_t(pin), 0);
    }
    esp_lcd_i80_bus_config_t config{};
    // Border is held at zero on this panel; use it for the mandatory dummy DC
    // signal, with every DC phase zero. Never steal an unrelated S31 GPIO.
    config.dc_gpio_num = gpio_num_t(epd_border);
    config.wr_gpio_num = gpio_num_t(epd_xcl);
    config.clk_src = LCD_CLK_SRC_PLL160M;
    config.bus_width = 8;
    config.max_transfer_bytes = LineBytes;
    config.dma_burst_size = 32;
    for (unsigned i = 0; i < epd_data.size(); ++i)
        config.data_gpio_nums[i] = gpio_num_t(epd_data[i]);
    esp_err_t error = esp_lcd_new_i80_bus(&config, &bus_);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "i80 bus: %s", esp_err_to_name(error));
        return false;
    }
    esp_lcd_panel_io_i80_config_t panel{};
    panel.cs_gpio_num = gpio_num_t(epd_xstl);
    panel.pclk_hz = 24000000;
    panel.trans_queue_depth = 2;
    panel.on_color_trans_done = dma_done;
    panel.user_ctx = this;
    panel.lcd_cmd_bits = 8;
    panel.lcd_param_bits = 8;
    // Paperboy's rising sampling edge, idle high, no data byte/bit permutation.
    panel.flags.pclk_idle_low = 0;
    panel.flags.pclk_active_neg = 0;
    error = esp_lcd_new_panel_io_i80(bus_, &panel, &io_);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "i80 IO: %s", esp_err_to_name(error));
        return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        dma_[i] = static_cast<uint8_t *>(
            esp_lcd_i80_alloc_draw_buffer(io_, LineBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        fb_[i] = static_cast<uint8_t *>(
            heap_caps_calloc(1, VideoBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!dma_[i] || !fb_[i])
            return false;
        memset(dma_[i], 0, LineBytes);
    }
    state_ = static_cast<uint8_t *>(
        heap_caps_calloc(1, StateBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    requests_ = xQueueCreate(1, sizeof(Request));
    completed_ = xSemaphoreCreateBinary();
    if (!state_ || !requests_ || !completed_)
        return false;
    if (xTaskCreatePinnedToCore(task_entry, "emu_scan", 4096, this, 3, &task_, 1) != pdPASS)
        return false;
    ESP_LOGI(TAG,
             "independent Paperboy scan ready: panel=%dx%d video=%dx%d@%d,%d pclk=24MHz drive=4 "
             "fields fb=%u state=%u line=%u bytes",
             PanelWidth, PanelHeight, VideoWidth, VideoHeight, VideoX, VideoY,
             unsigned(2 * VideoBytes), unsigned(StateBytes), unsigned(2 * LineBytes));
    return true;
}
bool PaperDisplay::wait_dma() {
    const int64_t start = esp_timer_get_time();
    while (!dma_finished_.load(std::memory_order_acquire)) {
        if (esp_timer_get_time() - start > 2000) {
            gpio_set_level(gpio_num_t(epd_xoe), 0);
            ESP_LOGE(TAG, "line DMA timeout; output disabled");
            healthy_.store(false);
            return false;
        }
    }
    return true;
}
bool PaperDisplay::row(uint8_t *data) {
    if (!wait_dma())
        return false;
    // Same latch/gate order as MSG. Latch the preceding transfer, then shift
    // the next row while the current gate row is active.
    gpio_set_level(gpio_num_t(epd_ckv), 0);
    gpio_set_level(gpio_num_t(epd_xle), 1);
    gpio_set_level(gpio_num_t(epd_xle), 0);
    gpio_set_level(gpio_num_t(epd_ckv), 1);
    dma_finished_.store(false, std::memory_order_release);
    const esp_err_t error = esp_lcd_panel_io_tx_color(io_, -1, data, LineBytes);
    if (error != ESP_OK) {
        gpio_set_level(gpio_num_t(epd_xoe), 0);
        healthy_.store(false);
        ESP_LOGE(TAG, "line submit: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}
bool PaperDisplay::frame(const uint8_t *image, bool white, bool video) {
    if (!wait_dma())
        return false;
    gpio_set_level(gpio_num_t(epd_xoe), 1);
    // Paperboy frame start: gate reset, start pulse, two lead-in clocks.
    gpio_set_level(gpio_num_t(epd_ckv), 1);
    esp_rom_delay_us(7);
    gpio_set_level(gpio_num_t(epd_spv), 0);
    esp_rom_delay_us(10);
    gpio_set_level(gpio_num_t(epd_ckv), 0);
    gpio_set_level(gpio_num_t(epd_ckv), 1);
    esp_rom_delay_us(8);
    gpio_set_level(gpio_num_t(epd_spv), 1);
    esp_rom_delay_us(10);
    gpio_set_level(gpio_num_t(epd_ckv), 0);
    for (unsigned i = 0; i < 2; ++i) {
        gpio_set_level(gpio_num_t(epd_ckv), 1);
        gpio_set_level(gpio_num_t(epd_ckv), 0);
    }
    if (video) {
        memset(dma_[0], 0, LineBytes);
        memset(dma_[1], 0, LineBytes);
        for (int y = 0; y < VideoY - 2; ++y)
            if (!row(dma_[0]))
                return false;
        dma_front_ = 0;
        uint8_t *state = state_;
        const uint8_t *source = fb_[front_.load(std::memory_order_acquire)];
        // Retain MSG's two-row pipeline and distribute 432 pixels across
        // three row transfers. Each completed drive row is scanned 3 times.
        for (int y = 0; y < VideoHeight; ++y) {
            uint8_t *output = dma_[!dma_front_] + VideoX / 4 + (y % 3) * (144 / 4);
            for (int group = 0; group < 144 / 8; ++group) {
                uint8_t incoming = *source++;
                for (int byte = 0; byte < 2; ++byte) {
                    uint8_t packed = 0;
                    for (int pair = 0; pair < 2; ++pair) {
                        packed = (packed << 4) | drive_pair(*state++, incoming >> 6);
                        incoming <<= 2;
                    }
                    *output++ = packed;
                }
            }
            if (y % 3 == 2)
                dma_front_ = !dma_front_;
            if (!row(dma_[dma_front_]))
                return false;
        }
        if (!row(dma_[dma_front_]) || !row(dma_[dma_front_]))
            return false;
        if (!wait_dma())
            return false;
        memset(dma_[0], 0, LineBytes);
        for (int y = VideoY + VideoHeight; y < PanelHeight; ++y)
            if (!row(dma_[0]))
                return false;
    } else {
        for (int y = 0; y < PanelHeight; ++y) {
            uint8_t *out = dma_[dma_front_];
            memset(out, 0, LineBytes);
            if (white)
                memset(out, 0xaa, PanelWidth / 4);
            else if (image) {
                const uint8_t *in = image + y * (PanelWidth / 8);
                for (int x = 0; x < PanelWidth / 8; ++x) {
                    out[x * 2] = image_drive(in[x] >> 4);
                    out[x * 2 + 1] = image_drive(in[x] & 15);
                }
            }
            if (!row(out))
                return false;
            dma_front_ = !dma_front_;
        }
        if (!wait_dma())
            return false;
    }
    if (!wait_dma())
        return false;
    memset(dma_[0], 0, LineBytes);
    if (!row(dma_[0]) || !wait_dma())
        return false;
    gpio_set_level(gpio_num_t(epd_xoe), 0);
    gpio_set_level(gpio_num_t(epd_ckv), 0);
    return true;
}
void PaperDisplay::task_entry(void *arg) {
    static_cast<PaperDisplay *>(arg)->run();
}
void PaperDisplay::run() {
    bool running = false;
    int64_t last_yield = esp_timer_get_time(), last_log = last_yield;
    uint32_t fields = 0;
    uint64_t scan_us = 0;
    for (;;) {
        Request request{};
        if (xQueueReceive(requests_, &request, running ? 0 : portMAX_DELAY) == pdTRUE) {
            video_active_.store(false);
            running = false;
            bool ok = healthy_.load();
            if (request.mode == Mode::Stop) {
                ok = hardware_->power(false) && ok;
            } else if (ok) {
                ok = hardware_->power(true);
                if (request.mode == Mode::Image) {
                    for (unsigned i = 0; ok && i < ImageFields; ++i) {
                        ok = frame(nullptr, true, false);
                        vTaskDelay(1);
                    }
                    for (unsigned i = 0; ok && i < ImageFields; ++i) {
                        ok = frame(request.image, false, false);
                        vTaskDelay(1);
                    }
                    ok = hardware_->power(false) && ok;
                } else {
                    memset(state_, 0, StateBytes);
                    memset(fb_[0], 0, VideoBytes);
                    memset(fb_[1], 0, VideoBytes);
                    front_.store(0);
                    flip_pending_.store(false);
                    running = ok;
                    video_active_.store(ok);
                }
            }
            if (!ok) {
                healthy_.store(false);
                hardware_->power(false);
            }
            command_ok_ = ok;
            xSemaphoreGive(completed_);
        }
        if (!running)
            continue;
        if (flip_pending_.load(std::memory_order_acquire)) {
            front_.store(!front_.load(), std::memory_order_release);
            flip_pending_.store(false, std::memory_order_release);
        }
        const int64_t begin = esp_timer_get_time();
        if (!frame(nullptr, false, true)) {
            running = false;
            video_active_.store(false);
            healthy_.store(false);
            hardware_->power(false);
            continue;
        }
        scan_us += esp_timer_get_time() - begin;
        ++fields;
        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 5000000) {
            ESP_LOGI(TAG, "video fields=%lu avg_scan=%lu us window=%lu ms", (unsigned long)fields,
                     (unsigned long)(scan_us / fields), (unsigned long)((now - last_log) / 1000));
            last_log = now;
            fields = 0;
            scan_us = 0;
        }
        // Let IDLE1 run without relaxing watchdogs. OE is already low at the
        // boundary; no pixel is held driven during this scheduler interval.
        if (now - last_yield >= 100000) {
            vTaskDelay(1);
            last_yield = esp_timer_get_time();
        }
    }
}
bool PaperDisplay::command(Mode mode, const uint8_t *image) {
    if (!task_)
        return false;
    const Request request{mode, image};
    if (xQueueSend(requests_, &request, pdMS_TO_TICKS(1000)) != pdTRUE)
        return false;
    // Image memory remains owned by the caller until the worker acknowledges.
    if (xSemaphoreTake(completed_, pdMS_TO_TICKS(10000)) != pdTRUE) {
        gpio_set_level(gpio_num_t(epd_xoe), 0);
        ESP_LOGE(TAG, "scan worker unresponsive; restarting");
        // No return: a late worker must never read a reused/freed image.
        esp_restart();
    }
    return command_ok_;
}
bool PaperDisplay::image(const uint8_t *image) {
    return image && command(Mode::Image, image);
}
bool PaperDisplay::video() {
    return command(Mode::Video);
}
bool PaperDisplay::stop() {
    return command(Mode::Stop);
}
bool PaperDisplay::present(const uint8_t *lcd) {
    if (!healthy_.load() || !video_active_.load() || flip_pending_.load(std::memory_order_acquire))
        return false;
    encode_lcd(lcd, fb_[!front_.load(std::memory_order_acquire)]);
    flip_pending_.store(true, std::memory_order_release);
    return true;
}
} // namespace emulator
