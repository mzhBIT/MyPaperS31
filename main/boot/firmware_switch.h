#pragma once
#include <esp_err.h>

namespace firmware_switch {
// Read-only header/identity check. Full image validation is performed by select.
esp_err_t check_emulator();
esp_err_t select_emulator();
esp_err_t select_system();
} // namespace firmware_switch
