#pragma once
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/SystemFont.h"
#include <M5Unified.h>

/** Simple placeholder — shows a label, tap to pop back. */
class PlaceholderActivity : public Activity {
    const char *_label;
public:
    PlaceholderActivity(const char *l) : _label(l) {}
    void on_create() override {
        M5.Display.startWrite();
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK);
        system_font::draw_center(M5.Display, _label, M5.Display.width()/2, M5.Display.height()/2 - 48);
        system_font::draw_center(M5.Display, "Tap to go back", M5.Display.width()/2, M5.Display.height()/2 + 12);
        M5.Display.display();
        M5.Display.endWrite();
    }
    bool on_touch(const TouchEvent &event) override {
        if (event.type != TouchEventType::Tap) return false;
        ESP_LOGI("PLACE", "pop!");
        ActivityManager::instance().pop();
        return true;
    }

    bool on_key(int key) override {
        if (key == APP_KEY_BACK || key == APP_KEY_OK) {
            ActivityManager::instance().pop();
            return true;
        }
        return false;
    }
};
