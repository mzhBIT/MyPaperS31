#include "boards/Board.h"
#include "Display.h"
#include "storage/storage_sdcard.h"
#include "storage/app_paths.h"
#include "frontlight/frontlight.h"
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <sdkconfig.h>
#if !CONFIG_IDF_TARGET_ESP32S31
#error "MyPaperS31 board compiled for the wrong target"
#endif
namespace mypaper::board {
namespace s31 {
m5gfx::M5GFX& display() { return M5.Display; }
}
HardwareInitResult initialize_hardware(bool usb_transfer_mode) {
    if (usb_transfer_mode) return {ESP_ERR_NOT_SUPPORTED, false};
    ESP_LOGI("BOARD", "S31 heap policy: malloc_internal_max=%d reserve_internal=%d bytes",
             CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL);
    if (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL != 0) {
        ESP_LOGW("BOARD", "S31 requires PSRAM-first malloc: small book metadata may exhaust internal SRAM");
    }
    if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        ESP_LOGE("BOARD", "S31 display requires PSRAM; none detected");
        return {ESP_ERR_NOT_SUPPORTED, false};
    }
    const bool display_ready = s31::display().init_without_reset(false);
    M5.Rtc.begin();
    const esp_err_t sd_status = storage_sdcard_mount();
    if (sd_status == ESP_OK) app_paths::ensure_data_dir();
    if (display_ready) s31::display().setRotation(0);
    if (display_ready) frontlight_init();
    if (display_ready) M5.Power.begin();
    return {sd_status, display_ready};
}

const Profile& profile() {
    // Compiled capabilities only; successful initialization is reported separately.
    static constexpr Profile p{
        BoardId::MyPaperS31, "MyPaperS31", "esp32s31", "FT6336U", "BH1098G",
        1216, 684, 8, bit(Capability::Display) | bit(Capability::Touch) |
                      bit(Capability::SdCard) | bit(Capability::Wifi) |
                      bit(Capability::Frontlight) | bit(Capability::FrontlightCct) |
                      bit(Capability::BatteryGauge) | bit(Capability::Rtc), false
    };
    return p;
}
} // namespace mypaper::board
