#pragma once

// Pico Reader's portrait design coordinates. Rendering and hit testing share
// these values; the P4/S31 panel is 684 x 1216 in portrait orientation.
namespace bookshelf_layout {
struct Rect {
    int x, y, w, h;
    constexpr bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
enum class Style { Acrylic = 2, DarkRail = 1, List = 5 };
constexpr int width = 684, height = 1216;
// Pico uses 100..200% (default 120%). Our existing 24..48px setting
// represents the same range; 28px gives approximately the original default.
// Ordinary UI text adds 2px before applying scale; large titles stay fixed.
constexpr int ui_font_pixels(int base, int system_pixels) {
    const int px = base + 2;
    return px < 46 ? (px * system_pixels + 12) / 24 : px;
}
constexpr Rect manage{442, 94, 97, 54}, import{551, 94, 97, 54};
constexpr Rect previous{194, 1032, 64, 60}, next{426, 1032, 64, 60};
constexpr int capacity(Style s) { return s == Style::List ? 4 : 9; }
constexpr Rect card(Style s, int slot) {
    return s == Style::List ? Rect{36, 216 + slot * 197, 612, 197}
        : Rect{40 + slot % 3 * 205, 224 + slot / 3 * 282, 193, 260};
}
constexpr Rect cover(Style s, int slot) {
    const auto r = card(s, slot);
    return s == Style::List ? Rect{71, r.y + 20, 120, 156}
        : Rect{r.x + 14, r.y, 164, 214};
}
constexpr Rect rail(int row) { return {20, 360 + row * 282, 644, 90}; }
}
