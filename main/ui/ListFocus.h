#pragma once

#include <algorithm>
#include <cstdint>

// List navigation focus only. Selected values, playback state, buttons and
// tabs are separate concepts and must keep their own state indicators.
namespace list_focus {
struct Style {
    int width = 5;
    int left_inset = 2;
    int vertical_inset = 16;
    int text_gap = 12;
    uint16_t color = 0x0000;
};
inline constexpr Style style{};

inline int text_inset() {
    return style.left_inset + style.width + style.text_gap;
}

struct Rect { int x, y, w, h; };
inline Rect geometry(int left, int top, int row_height) {
    if (row_height <= 0) return {left, top, 0, 0};
    const int padding = std::min(style.vertical_inset, row_height / 4);
    return {left + style.left_inset, top + padding, style.width,
            row_height - padding * 2};
}

// Call after clearing the row, before drawing its text. Unfocused rows are not
// erased here: doing so could overwrite thumbnails or independent indicators.
template <typename Graphics>
inline void draw(Graphics &gfx, int left, int top, int row_height, bool focused) {
    if (!focused || row_height <= 0) return;
    const Rect r = geometry(left, top, row_height);
    gfx.fillRect(r.x, r.y, r.w, r.h, style.color);
}
}  // namespace list_focus
