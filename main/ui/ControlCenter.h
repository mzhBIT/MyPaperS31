#pragma once
#include "input/touch_input.h"

// Main-thread global edge gesture; consumed gestures never reach applications.
bool control_center_handle_touch(const TouchEvent& event);
void control_center_service();
