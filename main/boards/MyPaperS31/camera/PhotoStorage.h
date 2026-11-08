#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>

namespace mypaper::board::s31::camera {
constexpr size_t photo_path_capacity = 128;
// Source is a completed, stable RGB565 big-endian frame. Stream RGB scanlines
// to baseline JPEG without allocating another full-size RGB888 image.
esp_err_t save_rgb565_photo(const uint8_t *rgb, int width, int height,
    char *saved_path, size_t capacity, bool (*cancelled)(void*), void *context);
}
