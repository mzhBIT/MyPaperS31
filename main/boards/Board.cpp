#include "Board.h"
#include <esp_log.h>

namespace mypaper::board {
bool supports(Capability capability) {
    return (profile().driver_capabilities & bit(capability)) != 0;
}
void log_profile() {
    const auto& p = profile();
    ESP_LOGI("BOARD", "%s target=%s panel=%ux%u bus=%u touch=%s audio=%s bootstrap=%d",
             p.name, p.target, unsigned(p.panel_width), unsigned(p.panel_height),
             unsigned(p.epd_bus_width), p.touch_controller, p.audio_backend, p.bootstrap_only);
}
} // namespace mypaper::board
