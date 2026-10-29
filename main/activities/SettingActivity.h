#pragma once

void launch_settings();
void switch_to_reading_settings();

// Returns true when a default network exists. Otherwise shows a short guide
// activity and returns false so the caller does not start its network stack.
bool ensure_wifi_configured_or_show_guide();
