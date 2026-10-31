#pragma once
#include <M5Unified.h>
#include <sdkconfig.h>

namespace battery_status {
inline bool available() {
#if CONFIG_IDF_TARGET_ESP32S31
    return M5.Power.getBatteryLevel() >= 0;
#else
    return false;
#endif
}
inline bool charging() {
#if CONFIG_IDF_TARGET_ESP32S31
    // Use the gauge's measured net charging current, not merely USB power or
    // the charger's enable bit. Full/idle and a discharging battery get no mark.
    return M5.Power.isCharging();
#else
    return false;
#endif
}
// Solid 12x20 lightning bolt; independent of font coverage and UI text size.
inline void draw_charging_mark(int x, int y) {
    M5.Display.fillTriangle(x+7, y, x, y+11, x+7, y+11, TFT_BLACK);
    M5.Display.fillTriangle(x+5, y+8, x+12, y+8, x+3, y+20, TFT_BLACK);
}
// Fill only the existing icon interior; no percentage text or independent
// refresh/timer. P4 keeps its original placeholder until a gauge is adapted.
inline void draw_fill(int x, int y, int width, int height) {
#if CONFIG_IDF_TARGET_ESP32S31
    const int level = M5.Power.getBatteryLevel();
    if (level < 0) {
        // Dash distinguishes unavailable from a valid empty battery.
        M5.Display.drawFastHLine(x + width / 3, y + height / 2, width / 3, TFT_BLACK);
    } else if (level > 0) {
        const int filled = (width * level + 99) / 100;
        M5.Display.fillRect(x, y, filled, height, TFT_BLACK);
    }
#else
    (void)x; (void)y; (void)width; (void)height;
#endif
}
}
