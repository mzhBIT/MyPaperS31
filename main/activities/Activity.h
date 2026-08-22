#pragma once

#include "input/touch_input.h"

enum AppKey {
    APP_KEY_NONE = 0,
    APP_KEY_UP,
    APP_KEY_DOWN,
    APP_KEY_OK,
    APP_KEY_BACK,
    APP_KEY_EXTRA,
};

class Activity {
public:
    virtual ~Activity() = default;
    virtual void on_create() = 0;
    virtual void on_show() {}
    virtual void on_hide() {}
    virtual void loop() {}
    virtual bool on_touch(const TouchEvent &/*event*/) { return false; }
    virtual bool allow_system_back() const { return true; }
    virtual bool on_key(int /*gesture*/) { return false; }
};
