#pragma once
#include <array>
#include <cstdint>
#include <esp_err.h>

namespace mypaper::board::s31::standby {
inline constexpr std::array<uint8_t, 7> auto_standby_options{0, 1, 5, 10, 15, 30, 60};

// UI thread only. Zero disables automatic standby.
void init();
uint8_t auto_standby_minutes();
esp_err_t set_auto_standby_minutes(uint8_t minutes);
void note_user_activity();
void request();
void request_shutdown();
// UI thread only. Returns true after an entry attempt/wake, so the caller
// restarts its loop with fresh input and activity state.
bool service();
}
