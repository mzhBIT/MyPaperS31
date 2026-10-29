#pragma once
#include "ui/SystemFont.h"
#include "time/time_basis.h"
#include <ctime>

namespace status_clock {
constexpr int header_height = 72;
constexpr int left = 36, center_y = 40, pixels = 28;
inline std::string text() {
    char value[24] = "--:--";
    if (time_basis_system_clock_valid())
        time_basis_format_clock(std::time(nullptr), value, sizeof(value));
    return value;
}
inline void draw(lgfx::LGFXBase& gfx, const std::string& value) {
    system_font::draw_on_background(gfx, value, left,
        system_font::centered_y(value, center_y, pixels), pixels, TFT_BLACK, TFT_WHITE);
}
}
