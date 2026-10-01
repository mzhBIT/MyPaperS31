#include "boards/Board.h"
#include <sdkconfig.h>
#include <M5Unified.h>
#include <esp_log.h>
#include "storage/app_paths.h"
#include "storage/storage_sdcard.h"
#include "frontlight/frontlight.h"
#if !CONFIG_IDF_TARGET_ESP32P4
#error "MyPaperP4 board compiled for the wrong target"
#endif
namespace mypaper::board {
HardwareInitResult initialize_hardware(bool usb_transfer_mode) {
    // Preserve the validated order: SD sets VDD_IO_5 before EPD uses it.
    const esp_err_t sd_ret = usb_transfer_mode
        ? storage_sdcard_init_usb_medium() : storage_sdcard_mount();
    if (sd_ret == ESP_OK && !usb_transfer_mode) {
        app_paths::ensure_data_dir();
    } else if (sd_ret != ESP_OK) {
        ESP_LOGW("MyPaperP4", "%s failed before display init: %s",
                 usb_transfer_mode ? "SD raw-medium initialization" : "SD card mount",
                 esp_err_to_name(sd_ret));
    }
    auto cfg = M5.config();
    cfg.internal_imu = false;
    cfg.external_imu = false;
    cfg.internal_spk = false;
    cfg.internal_rtc = false;
    cfg.external_rtc = false;
    // HomeActivity submits the first frame; do not clear twice at startup.
    cfg.clear_display = false;
    M5.begin(cfg);
    const bool ready = M5.Display.width() && M5.Display.height();
    // Frontlight shares the EPD-owned I2C bus, so initialize only after M5.
    if (ready) frontlight_init();
    return {sd_ret, ready};
}

const Profile& profile() {
    static constexpr Profile p{
        BoardId::MyPaperP4, "MyPaperP4", "esp32p4", "GT911", "ES8311",
        1216, 684, 16,
        bit(Capability::Display) | bit(Capability::Touch) | bit(Capability::SdCard) |
        bit(Capability::Wifi) | bit(Capability::Speaker) | bit(Capability::Frontlight) |
        bit(Capability::UsbMsc)
#if CONFIG_MYPAPER_USB_AUDIO
        | bit(Capability::UsbAudio)
#endif
#if CONFIG_MYPAPER_USB_MODEM_PROBE
        | bit(Capability::Modem)
#endif
        , false
    };
    return p;
}
} // namespace mypaper::board
