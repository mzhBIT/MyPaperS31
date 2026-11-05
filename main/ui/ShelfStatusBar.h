#pragma once
#include "ui/SystemFont.h"
#include "ui/StatusClock.h"
#include "ui/BatteryStatus.h"
#include "ui/pico_shelf_icons.h"
#include "time/time_basis.h"
#include "wifi/wifi_setting.h"
#include <esp_wifi.h>
#include <algorithm>
#include <ctime>

// Shared with the original bookshelf status strip: 684px reference canvas.
// Keep font metrics, icon coverage rendering and geometry identical on home.
namespace shelf_status_bar {
constexpr int height = status_clock::header_height;
inline uint16_t gray(int v) { return ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3); }
inline std::string clock_text() {
    return status_clock::text();
}
inline void draw_clock(const std::string& clock) {
    status_clock::draw(M5.Display, clock);
}
inline void draw_wifi(bool enabled) {
    constexpr int size = 28;
    const int cx = M5.Display.width() - 114;
    if (!enabled) {
        M5.Display.fillRect(cx-size/2, 40-size/2, size, size, TFT_WHITE);
        return;
    }
    const int step = (72 << 16) / size, half = step / 2 - (1 << 15);
    // Keep the existing status-strip refresh cadence; no extra radio polling
    // task or refresh is needed for signal strength.
    wifi_ap_record_t ap{};
    const bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const int level = !connected ? 0 : ap.rssi >= -60 ? 4 :
                                      ap.rssi >= -70 ? 3 : ap.rssi >= -80 ? 2 : 1;
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
            // The original 72px mask has a dot and three separate arcs,
            // centred on (35.5, 59.5), with radii about 0, 15, 30 and 45.
            // Classify radially so the ends of each arc keep the same colour.
            const int dx = (sx >> 15) - 71, dy = (sy >> 15) - 119;
            const int radius2 = dx*dx + dy*dy;
            const int band = radius2 < 225 ? 1 : radius2 < 2025 ? 2 :
                             radius2 < 5625 ? 3 : 4;
            const int ink = band <= level ? 0 : 153;
            int blended = (255*(15-a)+ink*a+7)/15;
            row[x] = gray((blended >> 4) * 17);
        }
        M5.Display.pushImage(cx-size/2, 40-size/2+y, size, 1, row);
    }
}
inline void draw_battery() {
    const int x = M5.Display.width() - 78;
    M5.Display.drawRect(x, 29, 40, 22, TFT_BLACK);
    M5.Display.fillRect(x+42, 36, 5, 8, TFT_BLACK);
    battery_status::draw_fill(x+4, 33, 32, 14);
    // The right margin leaves room beside the cap without covering the fill.
    M5.Display.fillRect(x+51, 28, 17, 24, TFT_WHITE);
    if (battery_status::charging()) battery_status::draw_charging_mark(x+52, 30);
}
inline std::string draw() {
    const auto clock = clock_text();
    draw_clock(clock);
    draw_wifi(wifi_setting_is_enabled());
    draw_battery();
    return clock;
}
}
