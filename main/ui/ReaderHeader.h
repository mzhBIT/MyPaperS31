#pragma once
#include "ui/StatusClock.h"
#include "input/touch_input.h"
#include <algorithm>

namespace reader_header {
constexpr int height = status_clock::header_height;
inline void draw(lgfx::LGFXBase& gfx, int width, const std::string& title,
                 const std::string& progress) {
    gfx.fillRect(0, 0, width, height, TFT_WHITE);
    const auto clock = status_clock::text();
    status_clock::draw(gfx, clock);
    const int reserve = std::max({180,
        status_clock::left + system_font::measure(clock, status_clock::pixels) + 16,
        system_font::measure(progress) + 16});
    const auto label = system_font::ellipsize(title, std::max(0, width - 2 * reserve));
    const int size = system_font::pixel_size();
    system_font::draw_center(gfx, label, width / 2,
        system_font::centered_y(label, status_clock::center_y, size), TFT_BLACK);
    system_font::draw_right(gfx, progress, width - 5,
        system_font::centered_y(progress, status_clock::center_y, size), TFT_BLACK);
}
// UI thread only, after prefetch and with no overlay/refresh active.
inline void update_clock(std::string& previous) {
    if (M5.Display.displayBusy() || touch_input_needs_fast_poll()) return;
    const auto value = status_clock::text();
    if (previous.empty() || previous == value) { previous = value; return; }
    auto& gfx = M5.Display;
    const int y = std::max(0, std::min(
        system_font::centered_y(previous, status_clock::center_y, status_clock::pixels),
        system_font::centered_y(value, status_clock::center_y, status_clock::pixels)));
    const int width = std::max(system_font::measure(previous, status_clock::pixels),
                               system_font::measure(value, status_clock::pixels)) + 4;
    const auto mode = gfx.getEpdMode();
    gfx.startWrite();
    gfx.setEpdMode(lgfx::epd_mode_t::epd_text);
    gfx.fillRect(status_clock::left, y, width, height - y, TFT_WHITE);
    status_clock::draw(gfx, value);
    gfx.display(status_clock::left, y, width, height - y);
    gfx.endWrite();
    gfx.setEpdMode(mode);
    previous = value;
}
}
