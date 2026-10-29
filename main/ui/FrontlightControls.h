#pragma once
#include "frontlight/frontlight.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include <algorithm>
#include <cstdio>

// Shared values and drawing for Settings and Control Center. Temperature is a
// relative warm/cool blend, not a Kelvin measurement.
namespace frontlight_controls {
enum Kind { Brightness = 0, Temperature = 1 };
inline bool has_temperature() { return frontlight_get_color_temperature_fine() >= 0; }
inline int value(Kind kind) {
    return kind == Brightness ? frontlight_get_brightness() : frontlight_get_color_temperature_fine();
}
inline void preview(Kind kind, int next) {
    next = std::clamp(next, 0, 1000);
    if (kind == Brightness) frontlight_preview_brightness(next);
    else frontlight_preview_color_temperature(next);
}
inline int step(Kind kind) { return kind == Brightness ? 20 : 10; }
inline const char* title(Kind kind) {
    return ui_locale::text(kind == Brightness ? ui_locale::TextId::Brightness : ui_locale::TextId::ColorTemperature);
}
inline std::string label(Kind kind) {
    const int v = value(kind);
    if (kind == Temperature) {
        if (v == 0) return ui_locale::text(ui_locale::TextId::WarmLight);
        if (v == 500) return ui_locale::text(ui_locale::TextId::NeutralLight);
        if (v == 1000) return ui_locale::text(ui_locale::TextId::CoolLight);
    }
    char text[24];
    const int tenths = kind == Brightness ? v / 2 : v;
    std::snprintf(text, sizeof(text), "%d.%d%%", tenths / 10, tenths % 10);
    return text;
}
inline int from_x(int x, int left, int right) {
    return std::clamp((x - left) * 1000 / std::max(1, right - left), 0, 1000);
}
inline void draw_slider(lgfx::LGFXBase& gfx, Kind kind, int left, int right, int y) {
    const int x = left + (right - left) * value(kind) / 1000;
    gfx.fillRoundRect(left, y - 3, right - left, 6, 3, TFT_BLACK);
    gfx.fillCircle(x, y, 14, TFT_WHITE);
    gfx.drawCircle(x, y, 14, TFT_BLACK);
    gfx.fillCircle(x, y, 6, TFT_BLACK);
}
} // namespace frontlight_controls
