#include "boards/Board.h"
#include "Display.h"
#include "Pins.h"
#include "UsbSwitch.h"
#include "storage/storage_sdcard.h"
#include "storage/app_paths.h"
#include "frontlight/frontlight.h"
#include <M5Unified.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <sdkconfig.h>
#if !CONFIG_IDF_TARGET_ESP32S31
#error "MyPaperS31 board compiled for the wrong target"
#endif
namespace mypaper::board {
namespace s31 {
m5gfx::M5GFX& display() { return M5.Display; }

void usb_analog_switch_set(bool usb_device) {
    static bool initialized = false;
    const auto pin = static_cast<gpio_num_t>(usb_analog_switch);
    // Low routes the shared USB-C connector to HS UTMI for the virtual disk;
    // high keeps the normal USB Serial/JTAG path available.
    const int level = usb_device ? 0 : 1;
    if (!initialized) {
        gpio_reset_pin(pin);
        // Latch the level before enabling the output driver so the switch never
        // sees a spurious low pulse (which would expose the MSC PHY).
        gpio_set_level(pin, level);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        initialized = true;
    } else {
        gpio_set_level(pin, level);
    }
}
} // namespace s31

HardwareInitResult initialize_hardware(bool usb_transfer_mode) {
    ESP_LOGI("BOARD", "S31 heap policy: malloc_internal_max=%d reserve_internal=%d bytes",
             CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL);
    if (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL != 0) {
        ESP_LOGW("BOARD", "S31 requires PSRAM-first malloc: small book metadata may exhaust internal SRAM");
    }
    if (!heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        ESP_LOGE("BOARD", "S31 display requires PSRAM; none detected");
        return {ESP_ERR_NOT_SUPPORTED, false};
    }

    // Never leave the board in virtual-disk routing while the normal
    // application owns the SD card. USB transfer mode pulls this low in
    // usb_msc_mode immediately before TinyUSB is installed.
    s31::usb_analog_switch_set(false);

    const bool display_ready = s31::display().init_without_reset(false);
    M5.Rtc.begin();

    const esp_err_t sd_status = usb_transfer_mode
        ? storage_sdcard_init_usb_medium() : storage_sdcard_mount();
    if (sd_status == ESP_OK && !usb_transfer_mode) app_paths::ensure_data_dir();
    if (sd_status != ESP_OK) {
        ESP_LOGW("BOARD", "%s failed before UI: %s",
                 usb_transfer_mode ? "SD raw-medium initialization" : "SD card mount",
                 esp_err_to_name(sd_status));
    }

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
                      bit(Capability::Speaker) |
                      bit(Capability::Frontlight) | bit(Capability::FrontlightCct) |
                      bit(Capability::BatteryGauge) | bit(Capability::Rtc) |
                      bit(Capability::UsbMsc) | bit(Capability::UsbAudio), false
    };
    return p;
}
} // namespace mypaper::board
