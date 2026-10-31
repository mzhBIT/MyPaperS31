#pragma once
#include "S31Hardware.h"
#include "VideoState.h"
#include <atomic>
#include <esp_lcd_io_i80.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace emulator {
// Independent MSG-style line streamer. Does not instantiate Panel_EPD_2D,
// read any reader waveform, or allocate/preheat whole drive frames.
class PaperDisplay {
  public:
    bool init(S31Hardware &hardware);
    bool image(const uint8_t *physical_white_bits);
    bool video();
    bool present(const uint8_t *lcd);
    bool stop();
    bool healthy() const {
        return healthy_.load();
    }

  private:
    enum class Mode { Image, Video, Stop };
    struct Request {
        Mode mode;
        const uint8_t *image;
    };
    bool command(Mode, const uint8_t * = nullptr);
    bool frame(const uint8_t *image, bool white, bool video);
    bool row(uint8_t *);
    bool wait_dma();
    void run();
    static void task_entry(void *arg);
    static bool IRAM_ATTR dma_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *,
                                   void *);
    S31Hardware *hardware_ = nullptr;
    esp_lcd_i80_bus_handle_t bus_ = nullptr;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    uint8_t *dma_[2]{};
    uint8_t *fb_[2]{};
    uint8_t *state_ = nullptr;
    QueueHandle_t requests_ = nullptr;
    SemaphoreHandle_t completed_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> dma_finished_{true}, healthy_{true}, flip_pending_{false},
        video_active_{false};
    std::atomic<unsigned> front_{0};
    bool command_ok_ = false;
    unsigned dma_front_ = 0;
};
} // namespace emulator
