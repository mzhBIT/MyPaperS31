#pragma once

#include <cstddef>
#include <cstdint>

// Set to 0 to restore the original server-side scale=2 download and direct
// M5GFX image path for A/B comparisons.
#ifndef MYPAPER_AMAP_LOW_RES_PIPELINE
#define MYPAPER_AMAP_LOW_RES_PIPELINE 1
#endif

class AmapGrayFrame {
public:
    AmapGrayFrame() = default;
    ~AmapGrayFrame();

    AmapGrayFrame(const AmapGrayFrame &) = delete;
    AmapGrayFrame &operator=(const AmapGrayFrame &) = delete;

    bool load_png(const char *path, uint16_t output_width, uint16_t output_height);
    void reset();

    const uint8_t *data() const { return _data; }
    uint16_t width() const { return _width; }
    uint16_t height() const { return _height; }
    uint8_t nearest_scale() const { return _nearest_scale; }
    bool valid() const { return _data != nullptr; }

private:
    uint8_t *_data = nullptr;
    uint16_t _width = 0;
    uint16_t _height = 0;
    uint8_t _nearest_scale = 0;
};
