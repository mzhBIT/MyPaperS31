#include "usb/usb_msc_mode.h"

#include "input/touch_input.h"
#include "storage/storage_sdcard.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "usb/usb_transfer_boot.h"

#include <M5Unified.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tinyusb_msc.h>

namespace {

const char *TAG = "USB_MSC";

enum class UiState : uint8_t {
    Waiting,
    Connected,
    Ejected,
    Error,
};

std::atomic<UiState> s_state{UiState::Waiting};
std::atomic<bool> s_dirty{false};
char s_error[96] = {};
tinyusb_msc_storage_handle_t s_storage = nullptr;
std::atomic<bool> s_exit_confirmation{false};

const char *usb_speed_text(tusb_speed_t speed)
{
    switch (speed) {
    case TUSB_SPEED_HIGH: return "High-Speed (480 Mbit/s)";
    case TUSB_SPEED_FULL: return "Full-Speed (12 Mbit/s)";
    case TUSB_SPEED_LOW: return "Low-Speed (1.5 Mbit/s)";
    default: return "unknown";
    }
}

const char *state_text(UiState state)
{
    switch (state) {
    case UiState::Waiting: return ui_locale::text(ui_locale::TextId::UsbWaitingForComputer);
    case UiState::Connected: return ui_locale::text(ui_locale::TextId::UsbConnectedPcOwnsSd);
    case UiState::Ejected: return ui_locale::text(ui_locale::TextId::UsbSafelyEjected);
    case UiState::Error: return s_error[0] ? s_error : ui_locale::text(ui_locale::TextId::UsbStorageError);
    }
    return ui_locale::text(ui_locale::TextId::UsbTransfer);
}

void set_state(UiState state)
{
    if (state != UiState::Connected) {
        s_exit_confirmation.store(false, std::memory_order_release);
    }
    s_state.store(state, std::memory_order_release);
    s_dirty.store(true, std::memory_order_release);
}

void device_event(tinyusb_event_t *event, void *)
{
    if (!event) return;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        set_state(UiState::Connected);
        const tusb_speed_t speed = tud_speed_get();
        ESP_LOGI(TAG, "USB host configured the MSC device: negotiated=%s enum=%d",
                 usb_speed_text(speed), (int)speed);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        // Without a VBUS monitor detach and suspend are not used to switch the
        // application back to its normal SD owner. The dedicated mode remains
        // active until the user explicitly exits.
        set_state(UiState::Waiting);
        ESP_LOGI(TAG, "USB host detached");
    }
}

void storage_event(tinyusb_msc_storage_handle_t, tinyusb_msc_event_t *event, void *)
{
    if (!event) return;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE) {
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            set_state(UiState::Ejected);
            ESP_LOGI(TAG, "Host safely ejected the SD card");
        } else {
            set_state(UiState::Connected);
        }
    } else if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED ||
               event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED ||
               event->id == TINYUSB_MSC_EVENT_FORMAT_FAILED) {
        std::snprintf(s_error, sizeof(s_error), ui_locale::text(ui_locale::TextId::SdMountErrorFormat), (int)event->id);
        set_state(UiState::Error);
    }
}

void draw_page()
{
    const int w = M5.Display.width();
    const int h = M5.Display.height();
    const int line_h = system_font::line_height();
    const int button_h = 90;
    const int button_y = h - button_h - 30;

    M5.Display.startWrite();
    M5.Display.setDither(false);
    M5.Display.setEpdMode(epd_mode_t::epd_text);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::UsbTransfer), w / 2, 28, TFT_BLACK);
    M5.Display.drawFastHLine(0, 28 + line_h + 12, w, TFT_BLACK);

    system_font::draw_center(M5.Display, state_text(s_state.load(std::memory_order_acquire)),
                             w / 2, 170, TFT_BLACK);
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::DeviceSdCard), w / 2, 240, TFT_BLACK);
    if (s_exit_confirmation.load(std::memory_order_acquire)) {
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::UsbNotSafelyEjected),
                                 w / 2, 350, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::UsbForceRestartHint), w / 2,
                                 350 + line_h + 8, TFT_BLACK);
    } else {
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::UsbEjectOnComputer), w / 2,
                                 350, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::UsbBeforeLeaving), w / 2,
                                 350 + line_h + 8, TFT_BLACK);
    }

    M5.Display.drawRect(30, button_y, w - 60, button_h, TFT_BLACK);
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ExitUsbTransfer), w / 2,
                             button_y + (button_h - line_h) / 2, TFT_BLACK);
    M5.Display.display();
    M5.Display.endWrite();
}

void make_serial(char *out, size_t out_size)
{
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        std::snprintf(out, out_size, "MYPAPERP4");
        return;
    }
    std::snprintf(out, out_size, "%02X%02X%02X%02X%02X%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t start_msc()
{
    tinyusb_msc_driver_config_t driver_cfg = {};
    driver_cfg.callback = storage_event;
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver_cfg), TAG,
                        "install MSC driver failed");

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.medium.card = storage_sdcard_get_card();
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    storage_cfg.fat_fs.base_path = (char *)"/usbdata";
    storage_cfg.fat_fs.config.max_files = 2;
    storage_cfg.fat_fs.do_not_format = true;
    esp_err_t ret = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage);
    if (ret != ESP_OK) {
        tinyusb_msc_uninstall_driver();
        return ret;
    }

    static char serial[20];
    static const char language[] = {0x09, 0x04, 0x00};
    static const char *strings[] = {
        language,
        "MyPaperP4",
        "MyPaperP4 SD Card",
        serial,
        "SD Card",
    };
    make_serial(serial, sizeof(serial));

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(device_event);
    // Manual mode: the board is separately powered and VBUS is deliberately
    // isolated. Starting/stopping this dedicated mode substitutes for automatic
    // VBUS monitoring.
    config.phy.self_powered = false;
    config.phy.vbus_monitor_io = -1;
    // The EPD scan worker is pinned to CPU1. Keep USB enumeration and MSC
    // service on CPU0 so a display refresh cannot delay EP0 control requests.
    config.task = TINYUSB_TASK_CUSTOM(4096, 5, 0);
    config.descriptor.string = strings;
    config.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    ret = tinyusb_driver_install(&config);
    if (ret != ESP_OK) {
        tinyusb_msc_delete_storage(s_storage);
        s_storage = nullptr;
        tinyusb_msc_uninstall_driver();
        return ret;
    }

    uint32_t sectors = 0;
    uint32_t sector_size = 0;
    tinyusb_msc_get_storage_capacity(s_storage, &sectors);
    tinyusb_msc_get_storage_sector_size(s_storage, &sector_size);
    ESP_LOGI(TAG, "MSC ready port=HS buffer=%d bytes sectors=%lu sector_size=%lu capacity=%llu bytes",
             CONFIG_TINYUSB_MSC_BUFSIZE,
             (unsigned long)sectors, (unsigned long)sector_size,
             (unsigned long long)sectors * sector_size);
    return ESP_OK;
}

} // namespace

extern "C" esp_err_t usb_msc_mode_run(void)
{
    if (storage_sdcard_get_card() == nullptr) return ESP_ERR_INVALID_STATE;

    s_state.store(UiState::Waiting, std::memory_order_release);
    s_dirty.store(false, std::memory_order_release);
    s_exit_confirmation.store(false, std::memory_order_release);
    s_error[0] = '\0';
    draw_page();

    // Do not expose the USB pull-up while the initial Quality refresh is still
    // occupying the EPD worker. Windows requests the device descriptor as soon
    // as the device attaches and has a short EP0 timeout.
    M5.Display.waitDisplay();

    esp_err_t ret = start_msc();
    if (ret != ESP_OK) {
        std::snprintf(s_error, sizeof(s_error), ui_locale::text(ui_locale::TextId::UsbStartFailedFormat), esp_err_to_name(ret));
        set_state(UiState::Error);
        draw_page();
    }

    while (true) {
        touch_input_update();
        TouchEvent event;
        while (touch_input_pop(&event)) {
            bool exit_requested = event.type == TouchEventType::Back;
            if (event.type == TouchEventType::Tap) {
                const int h = M5.Display.height();
                exit_requested = event.y >= h - 120;
            }
            if (exit_requested) {
                const UiState state = s_state.load(std::memory_order_acquire);
                if (state == UiState::Connected &&
                    !s_exit_confirmation.load(std::memory_order_acquire)) {
                    s_exit_confirmation.store(true, std::memory_order_release);
                    draw_page();
                    continue;
                }
                ESP_LOGI(TAG, "User requested exit from USB transfer mode");
                const esp_err_t flush_ret = storage_sdcard_flush_usb_write_cache();
                if (flush_ret != ESP_OK) {
                    ESP_LOGE(TAG, "Final SD write cache flush failed: %s",
                             esp_err_to_name(flush_ret));
                }
                usb_transfer_boot_set(false);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }
        if (s_dirty.exchange(false, std::memory_order_acq_rel)) draw_page();
        storage_sdcard_log_usb_transfer_stats();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
