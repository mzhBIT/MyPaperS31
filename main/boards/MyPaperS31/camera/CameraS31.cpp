#include "CameraS31.h"
#include "gc2145_vga_settings.h"
#include "boards/MyPaperS31/Pins.h"
#include "lgfx/v1/platforms/esp32/EpdPeripheralGuard.hpp"
#include <M5Unified.h>
#include <esp_cam_ctlr_dvp.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <new>
#include <atomic>

namespace mypaper::board::s31::camera {
namespace {
constexpr const char *TAG = "CAMERA_S31";
constexpr uint8_t address = 0x3c;
constexpr size_t rgb_bytes = size_t(width) * height * 2;
std::atomic<bool> boot_camera_present{false};
}
struct Session::Impl {
    bool (*cancelled)(void*);
    void *context;
    esp_cam_ctlr_handle_t controller = nullptr;
    uint8_t *frame = nullptr, *scratch = nullptr;
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done = nullptr;
    unsigned requests = 0, warmup = 8;
    volatile size_t received = 0;
    bool enabled = false, started = false, configured = false, first = true;
    bool sensor_found = false;
    bool sensor_touched = false;
    bool captured = false;
    bool cancel() const { return cancelled && cancelled(context); }
    esp_err_t write(uint8_t reg, uint8_t value) {
        if (cancel()) return ESP_ERR_INVALID_STATE;
        return m5::In_I2C.writeRegister8(address, reg, value, 100000) ? ESP_OK : ESP_FAIL;
    }
    esp_err_t read(uint8_t reg, uint8_t &value) {
        if (cancel()) return ESP_ERR_INVALID_STATE;
        return m5::In_I2C.readRegister(address, reg, &value, 1, 100000) ? ESP_OK : ESP_FAIL;
    }
    static bool IRAM_ATTR next(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *arg) {
        auto *self = static_cast<Impl *>(arg);
        // Always provide a valid buffer with bk_buffer_dis. Only one frame is
        // published; any transaction started before task wake uses scratch.
        trans->buffer = ++self->requests == self->warmup + 1 ? self->frame : self->scratch;
        trans->buflen = rgb_bytes;
        return false;
    }
    static bool IRAM_ATTR finish(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *arg) {
        auto *self = static_cast<Impl *>(arg);
        if (trans->buffer != self->frame) return false;
        self->received = trans->received_size;
        BaseType_t wake = pdFALSE;
        xSemaphoreGiveFromISR(self->done, &wake);
        return wake == pdTRUE;
    }
};
Session::Session(bool (*cancelled)(void*), void *context) : _impl(nullptr) {
    void *memory = heap_caps_malloc(sizeof(Impl), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (memory) {
        _impl = new (memory) Impl;
        _impl->cancelled = cancelled;
        _impl->context = context;
        _impl->done = xSemaphoreCreateBinaryStatic(&_impl->done_storage);
    }
}
Session::~Session() {
    close();
    if (_impl) {
        if (_impl->done) vSemaphoreDelete(_impl->done);
        _impl->~Impl();
        heap_caps_free(_impl);
    }
}
esp_err_t Session::probe() {
    if (!_impl || !_impl->done) return ESP_ERR_NO_MEM;
    auto &self = *_impl;
    if (self.controller) return self.sensor_found ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (self.cancel()) return ESP_ERR_INVALID_STATE;
    // Read-only check of shared P05/P12. No camera action may reset touch/4G
    // or turn the screen supply off, even on an absent-camera board.
    uint8_t outputs[2]{};
    if (!m5::In_I2C.readRegister(io_address, 2, outputs, 2, 100000)) return ESP_FAIL;
    if (!(outputs[0] & (1u << 5)) || !(outputs[1] & (1u << 2))) return ESP_ERR_INVALID_STATE;

    esp_cam_ctlr_dvp_pin_config_t pins{};
    pins.data_width = CAM_CTLR_DATA_WIDTH_8;
    for (int i = 0; i < ESP_CAM_CTLR_DVP_DATA_SIG_NUM; ++i)
        pins.data_io[i] = i < 8 ? static_cast<gpio_num_t>(camera_data0 + i) : GPIO_NUM_NC;
    pins.pclk_io = static_cast<gpio_num_t>(camera_pclk);
    pins.vsync_io = static_cast<gpio_num_t>(camera_vsync);
    pins.de_io = static_cast<gpio_num_t>(camera_href);
    pins.xclk_io = static_cast<gpio_num_t>(camera_mclk);
    esp_cam_ctlr_dvp_config_t config{};
    config.clk_src = CAM_CLK_SRC_DEFAULT;
    config.h_res = width;
    config.v_res = height;
    config.input_data_color_type = CAM_CTLR_COLOR_RGB565;
    config.output_data_color_type = CAM_CTLR_COLOR_RGB565;
    config.cam_data_width = 8;
    config.xclk_freq = 20000000;
    config.dma_burst_size = 64;
    config.bk_buffer_dis = 1; // Allocate RGB only after successful PID probe.
    config.pin = &pins;
    auto result = esp_cam_new_dvp_ctlr(&config, &self.controller);
    if (result != ESP_OK) return result;
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t high = 0, low = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        result = self.write(0xfe, 0); // Select bank 0; no sensor reset/configuration.
        if (result == ESP_OK) result = self.read(0xf0, high);
        if (result == ESP_OK) result = self.read(0xf1, low);
        if (self.cancel()) return ESP_ERR_INVALID_STATE;
        if (result == ESP_OK && high == 0x21 && low == 0x45) {
            self.sensor_found = true;
            return ESP_OK;
        }
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "optional GC2145 not detected: PID=0x%02x%02x", high, low);
    return ESP_ERR_NOT_FOUND;
}
esp_err_t Session::open() {
    if (!_impl) return ESP_ERR_NO_MEM;
    auto &self = *_impl;
    if (self.configured) return ESP_OK;
    auto result = probe();
    if (result != ESP_OK) return result;
    self.sensor_touched = true; // Close must also clean up a partial register setup.
    for (const auto &entry : kGc2145Vga) {
        if (self.cancel()) return ESP_ERR_INVALID_STATE;
        if (entry.reg == 0xff) vTaskDelay(pdMS_TO_TICKS(entry.value));
        else if ((result = self.write(entry.reg, entry.value)) != ESP_OK) return result;
    }
    self.frame = static_cast<uint8_t *>(esp_cam_ctlr_alloc_buffer(self.controller, rgb_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    self.scratch = static_cast<uint8_t *>(esp_cam_ctlr_alloc_buffer(self.controller, rgb_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    if (!self.frame || !self.scratch) return ESP_ERR_NO_MEM;
    esp_cam_ctlr_evt_cbs_t callbacks{};
    callbacks.on_get_new_trans = Impl::next;
    callbacks.on_trans_finished = Impl::finish;
    result = esp_cam_ctlr_register_event_callbacks(self.controller, &callbacks, &self);
    if (result != ESP_OK) return result;
    self.configured = true;
    ESP_LOGI(TAG, "GC2145 ready: 640x480 RGB565_BE, nominal=15fps, MCLK=20MHz; RGB PSRAM=%u",
        unsigned(rgb_bytes*2));
    return ESP_OK;
}
esp_err_t Session::capture(uint8_t *packed, size_t capacity) {
    if (!_impl || !_impl->configured || !packed || capacity < packed_bytes) return ESP_ERR_INVALID_STATE;
    auto &self = *_impl;
    self.captured = false;
    if (self.cancel()) return ESP_ERR_INVALID_STATE;
    const int64_t start = esp_timer_get_time();
    while (xSemaphoreTake(self.done, 0) == pdTRUE) {}
    self.requests = 0;
    self.received = 0;
    self.warmup = self.first ? 8 : 1;
    // Disable after every frame to release the driver's PM lock. Sensor
    // configuration/MCLK remain alive, so exposure can settle between updates.
    auto result = esp_cam_ctlr_enable(self.controller);
    if (result != ESP_OK) return result;
    self.enabled = true;
    self.started = true; // start() changes FSM even if DMA setup fails.
    result = esp_cam_ctlr_start(self.controller);
    if (result == ESP_OK) {
        while (xSemaphoreTake(self.done, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (self.cancel()) { result = ESP_ERR_INVALID_STATE; break; }
            if (esp_timer_get_time()-start >= 5000000) { result = ESP_ERR_TIMEOUT; break; }
        }
    }
    const auto stop = esp_cam_ctlr_stop(self.controller);
    self.started = false;
    const auto disable = esp_cam_ctlr_disable(self.controller);
    self.enabled = false;
    if (result != ESP_OK) return result;
    if (stop != ESP_OK) return stop;
    if (disable != ESP_OK) return disable;
    if (self.received != rgb_bytes) return ESP_ERR_INVALID_SIZE;
    for (int y = 0; y < height; ++y) {
        if (self.cancel()) return ESP_ERR_INVALID_STATE;
        for (int x = 0; x < width; x += 2) {
            uint8_t shades[2];
            for (int p = 0; p < 2; ++p) {
                const size_t pos = (size_t(y)*width+x+p)*2;
                const unsigned rgb = unsigned(self.frame[pos])<<8 | self.frame[pos+1];
                const unsigned r = ((rgb>>11)&31)*255/31;
                const unsigned g = ((rgb>>5)&63)*255/63;
                const unsigned b = (rgb&31)*255/31;
                shades[p] = uint8_t(((77*r+150*g+29*b+128)>>8)>>4);
            }
            packed[(size_t(y)*width+x)/2] = uint8_t(shades[0]<<4 | shades[1]);
        }
    }
    ESP_LOGI(TAG, "preview frame: warmup=%u total=%lld ms", self.warmup,
        (esp_timer_get_time()-start)/1000);
    self.first = false;
    self.captured = true;
    return ESP_OK;
}
esp_err_t Session::save_photo(char *saved_path, size_t capacity) {
    if (!_impl || !_impl->configured || !_impl->captured) return ESP_ERR_INVALID_STATE;
    // Called by the owning worker, after DMA stops and the peripheral gate
    // is released. No subsequent capture can mutate this source meanwhile.
    return save_rgb565_photo(_impl->frame, width, height, saved_path, capacity,
        _impl->cancelled, _impl->context);
}
void Session::close() {
    if (!_impl) return;
    auto &self = *_impl;
    if (self.started) { esp_cam_ctlr_stop(self.controller); self.started = false; }
    if (self.enabled) { esp_cam_ctlr_disable(self.controller); self.enabled = false; }
    if (self.sensor_touched) {
        // Cleanup must run even after cancellation. Do not use write(), whose
        // cancellation check intentionally rejects normal work at this point.
        m5::In_I2C.writeRegister8(address, 0xfe, 0, 100000);
        m5::In_I2C.writeRegister8(address, 0xf2, 0, 100000);
    }
    self.sensor_touched = false;
    self.sensor_found = false;
    if (self.controller) {
        esp_cam_ctlr_del(self.controller);
        self.controller = nullptr;
        gpio_reset_pin(static_cast<gpio_num_t>(camera_mclk));
    }
    heap_caps_free(self.frame); self.frame = nullptr;
    heap_caps_free(self.scratch); self.scratch = nullptr;
    self.configured = false;
    self.first = true;
    self.captured = false;
}
esp_err_t probe_at_boot() {
    const int64_t begin = esp_timer_get_time();
    lgfx::EpdPeripheralGuard hardware;
    Session session(nullptr, nullptr);
    const auto result = session.probe();
    session.close(); // Release controller/DMA descriptors/MCLK before home UI.
    boot_camera_present.store(result == ESP_OK);
    ESP_LOGI(TAG, "boot probe: result=%s present=%u time=%lld ms (no image buffers)",
        esp_err_to_name(result), unsigned(result == ESP_OK), (esp_timer_get_time()-begin)/1000);
    return result;
}
bool available() { return boot_camera_present.load(); }
}
