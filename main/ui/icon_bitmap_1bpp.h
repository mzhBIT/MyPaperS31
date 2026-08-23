#pragma once

#include <M5Unified.h>
#include <cstdint>

struct IconBitmap1bpp {
    const uint8_t *data;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint32_t data_size;
    int16_t anchor_x;
    int16_t anchor_y;
};

bool icon_bitmap_1bpp_valid(const IconBitmap1bpp &icon);

void draw_icon_1bpp(M5GFX &gfx,
                     const IconBitmap1bpp &icon,
                     int target_x,
                     int target_y,
                     uint16_t foreground = TFT_BLACK,
                     uint16_t background = TFT_WHITE,
                     int destination_width = 0,
                     int destination_height = 0);
