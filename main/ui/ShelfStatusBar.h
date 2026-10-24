#pragma once
#include "ui/SystemFont.h"
#include "ui/BatteryStatus.h"
#include "ui/pico_shelf_icons.h"
#include "time/time_basis.h"
#include "wifi/wifi_setting.h"
#include <algorithm>
#include <ctime>

// Shared with the original bookshelf status strip: 684px reference canvas.
// Keep font metrics, icon coverage rendering and geometry identical on home.
namespace shelf_status_bar {
constexpr int height = 72;
inline uint16_t gray(int v) { return ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3); }
inline std::string clock_text() {
    char clock[24] = "--:--";
    if (time_basis_system_clock_valid()) time_basis_format_clock(std::time(nullptr), clock, sizeof(clock));
    return clock;
}
inline void draw_clock(const std::string& clock) {
    system_font::draw_on_background(M5.Display, clock, 36,
        system_font::centered_y(clock, 40, 28), 28, TFT_BLACK, TFT_WHITE);
}
inline void draw_wifi(bool enabled) {
    constexpr int size = 28;
    const int cx = M5.Display.width() - 114;
    const int step = (72 << 16) / size, half = step / 2 - (1 << 15);
    const int ink = enabled ? 0 : 144;
    const auto* mask = pico_shelf_icons::wifi;
    for (int y = 0; y < size; ++y) {
        lgfx::rgb565_t row[size];
        for (int x = 0; x < size; ++x) {
            int sx = x * step + half, sy = y * step + half;
            int ix = sx >> 16, iy = sy >> 16;
            auto alpha = [&](int px, int py) {
                px = std::clamp(px, 0, 71); py = std::clamp(py, 0, 71);
                int pos = py * 72 + px;
                return (pos & 1) ? mask[pos / 2] & 15 : mask[pos / 2] >> 4;
            };
            int fx = sx & 65535, fy = sy & 65535;
            int upper = (alpha(ix,iy)*(65536-fx)+alpha(ix+1,iy)*fx)>>16;
            int lower = (alpha(ix,iy+1)*(65536-fx)+alpha(ix+1,iy+1)*fx)>>16;
            int a = (upper*(65536-fy)+lower*fy)>>16;
            int blended = (255*(15-a)+ink*a+7)/15;
            row[x] = gray((blended >> 4) * 17);
        }
        M5.Display.pushImage(cx-size/2, 40-size/2+y, size, 1, row);
    }
    if (!enabled) M5.Display.drawLine(cx-12, 28, cx+12, 52, gray(96));
}
inline void draw_battery() {
    const int x = M5.Display.width() - 78;
    M5.Display.drawRect(x, 29, 40, 22, TFT_BLACK);
    M5.Display.fillRect(x+42, 36, 5, 8, TFT_BLACK);
    battery_status::draw_fill(x+4, 33, 32, 14);
}
inline void draw() {
    draw_clock(clock_text());
    draw_wifi(wifi_setting_is_enabled());
    draw_battery();
}
}
