#pragma once

#include "ui/SystemFont.h"

// Application navigation header. Reader content keeps its compact/hidden header.
namespace header_layout {
constexpr int height = 76;
constexpr int separator_y = height - 1;
inline int item_y(int item_height) {
    return item_height < separator_y ? (separator_y - item_height) / 2 : 0;
}
inline int text_y() { return item_y(system_font::line_height()); }
// Half-open bounds: the first content row is not part of the header.
inline bool contains(int y, int header_height = height) {
    return y >= 0 && y < header_height;
}
inline bool left_hit(int x, int y, int button_width = 120,
                     int header_height = height) {
    return x >= 0 && x < button_width && contains(y, header_height);
}
inline bool right_hit(int x, int y, int screen_width, int button_width,
                      int header_height = height) {
    return x >= screen_width - button_width && x < screen_width &&
           contains(y, header_height);
}
inline void draw_separator(lgfx::LGFXBase &gfx) {
    gfx.drawFastHLine(0, separator_y, gfx.width(), TFT_BLACK);
}
} // namespace header_layout
