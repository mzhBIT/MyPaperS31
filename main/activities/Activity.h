#pragma once

#include "input/touch_input.h"

enum AppKey {
    APP_KEY_NONE = 0,
    APP_KEY_UP,
    APP_KEY_DOWN,
    APP_KEY_OK,
    APP_KEY_BACK,
    APP_KEY_EXTRA,
    APP_KEY_READER_MENU,
};

class Activity {
public:
    virtual ~Activity() = default;
    virtual void on_create() = 0;
    virtual void on_show() {}
    virtual void on_hide() {}
    // Modal overlays do not hide/re-enter the activity underneath them.
    virtual void on_overlay_dismiss(bool /*restore*/) { on_hide(); }
    virtual void loop() {}
    virtual bool on_touch(const TouchEvent &/*event*/) { return false; }
    virtual bool allow_system_back() const { return true; }
    virtual bool allow_system_home() const { return true; }
    virtual bool on_key(int /*gesture*/) { return false; }
    // Optional board-key semantics. Do not guess page navigation from an
    // arbitrary app's UP/DOWN focus keys or send BOOT to an unrelated action.
    virtual bool on_navigation_key(int /*direction*/) { return false; }
    virtual bool on_reader_menu_key() { return false; }
    virtual bool on_cover_home_key() { return false; }
    // Dedicated volume keys must not be interpreted as focus/page navigation.
    // Return true after applying the delta and updating this page's volume UI.
    virtual bool on_volume_key(int /*delta*/) { return false; }
    // Background-I/O barrier for manual light sleep; called on the UI thread.
    virtual bool can_standby() const { return true; }
    // Close live resource users before the system tears down Wi-Fi/peripherals.
    // Return false to cancel this power action without releasing those resources.
    virtual bool prepare_standby() { return true; }
    // Only the foreground reader supplies artwork; other applications use the system wallpaper.
    virtual const char *standby_book_cover_path() const { return nullptr; }
    virtual void on_standby_enter() { on_hide(); }
    virtual void on_standby_resume() {}
};
