#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include "PhotoStorage.h"

namespace mypaper::board::s31::camera {
constexpr int width = 640, height = 480;
constexpr size_t packed_bytes = size_t(width) * height / 2;
// Called once during normal system boot, before creating the home page.
// Only powers the camera clock long enough to read PID; no RGB allocations.
esp_err_t probe_at_boot();
bool available(); // Cached boot result; no I2C or allocation from UI.
// Worker-owned session; only callback context resides in internal SRAM. RGB
// frames use PSRAM and are not allocated until the optional sensor is found.
class Session {
public:
    explicit Session(bool (*cancelled)(void*), void *context);
    ~Session();
    esp_err_t probe();
    esp_err_t open();
    esp_err_t capture(uint8_t *packed, size_t capacity);
    esp_err_t save_photo(char *saved_path, size_t capacity);
    void close();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
private:
    struct Impl;
    Impl *_impl;
};
}
