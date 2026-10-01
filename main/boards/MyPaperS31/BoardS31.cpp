#include "boards/Board.h"
#include "Display.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <sdkconfig.h>
#if !CONFIG_IDF_TARGET_ESP32S31
#error "MyPaperS31 board compiled for the wrong target"
#endif
namespace mypaper::board {
namespace s31 {
m5gfx::M5GFX& display() { static m5gfx::M5GFX gfx; return gfx; }
}
HardwareInitResult initialize_hardware(bool usb_transfer_mode) {
    if (usb_transfer_mode) return {ESP_ERR_NOT_SUPPORTED, false};
    if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        ESP_LOGE("BOARD", "S31 display requires PSRAM; none detected");
        return {ESP_ERR_NOT_SUPPORTED, false};
    }
    return {ESP_ERR_NOT_SUPPORTED, s31::display().init_without_reset(false)};
}

const Profile& profile() {
    // Compiled capabilities only; successful initialization is reported separately.
    static constexpr Profile p{
        BoardId::MyPaperS31, "MyPaperS31", "esp32s31", "FT6336U", "BH1098G",
        1216, 684, 8, bit(Capability::Display) | bit(Capability::Touch), false
    };
    return p;
}
} // namespace mypaper::board
