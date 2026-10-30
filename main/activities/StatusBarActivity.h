#pragma once

#include "activities/Activity.h"
#include "ui/AppStatusBar.h"

// Shared by settings lists and their child pages. A custom loop must call the
// base loop when idle; diagnostic patterns deliberately suppress it.
class StatusBarActivity : public Activity {
public:
    void loop() override { refresh_header(); }
protected:
    void draw_header(const char* title) {
        _header_title = title;
        _status_bar.draw(_header_title);
    }
    void refresh_header() {
        if (!_header_title.empty()) _status_bar.refresh(_header_title.c_str());
    }
    static bool status_bar_touch(const TouchEvent& event) {
        return event.type == TouchEventType::Tap &&
            app_status_bar_layout::contains(event.x,event.y,M5.Display.width());
    }
private:
    app_status_bar::State _status_bar{true};
    std::string _header_title;
};
