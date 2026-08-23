#include "ui/icon_bitmap_1bpp.h"

#include <algorithm>

namespace {

bool bitmap_bit(const IconBitmap1bpp &icon, int x, int y)
{
    size_t offset = static_cast<size_t>(y) * icon.stride + static_cast<size_t>(x >> 3);
    return (icon.data[offset] & (0x80u >> (x & 7))) != 0;
}

}  // namespace

bool icon_bitmap_1bpp_valid(const IconBitmap1bpp &icon)
{
    if (icon.data == nullptr || icon.width == 0 || icon.height == 0) return false;
    uint16_t minimum_stride = static_cast<uint16_t>((icon.width + 7u) / 8u);
    return icon.stride >= minimum_stride &&
           icon.data_size >= static_cast<uint32_t>(icon.stride) * icon.height &&
           icon.anchor_x >= 0 && icon.anchor_x <= static_cast<int16_t>(icon.width) &&
           icon.anchor_y >= 0 && icon.anchor_y <= static_cast<int16_t>(icon.height);
}

void draw_icon_1bpp(M5GFX &gfx,
                     const IconBitmap1bpp &icon,
                     int target_x,
                     int target_y,
                     uint16_t foreground,
                     uint16_t background,
                     int destination_width,
                     int destination_height)
{
    if (!icon_bitmap_1bpp_valid(icon)) return;
    if (destination_width <= 0) destination_width = icon.width;
    if (destination_height <= 0) destination_height = icon.height;

    int scaled_anchor_x = (icon.anchor_x * destination_width + icon.width / 2) / icon.width;
    int scaled_anchor_y = (icon.anchor_y * destination_height + icon.height / 2) / icon.height;
    int left = target_x - scaled_anchor_x;
    int top = target_y - scaled_anchor_y;

    if (destination_width == icon.width && destination_height == icon.height &&
        icon.stride == (icon.width + 7u) / 8u) {
        gfx.drawBitmap(left, top, icon.data, icon.width, icon.height, foreground, background);
        return;
    }

    for (int dy = 0; dy < destination_height; ++dy) {
        int source_y = dy * icon.height / destination_height;
        int run_start = 0;
        bool run_foreground = bitmap_bit(icon, 0, source_y);
        for (int dx = 1; dx <= destination_width; ++dx) {
            int source_x = dx < destination_width ? dx * icon.width / destination_width : icon.width;
            bool foreground_pixel = dx < destination_width
                                        ? bitmap_bit(icon, source_x, source_y)
                                        : !run_foreground;
            if (dx == destination_width || foreground_pixel != run_foreground) {
                gfx.fillRect(left + run_start,
                             top + dy,
                             dx - run_start,
                             1,
                             run_foreground ? foreground : background);
                run_start = dx;
                run_foreground = foreground_pixel;
            }
        }
    }
}
