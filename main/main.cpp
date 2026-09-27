#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_hosted.h>
#include <driver/gpio.h>
#include <nvs_flash.h>
#include <M5Unified.h>

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/PhoneActivity.h"
#include "modem/ml307_voice.h"
#include "input/touch_input.h"
#include "storage/app_paths.h"
#include "storage/storage_sdcard.h"
#include "usb/usb_msc_mode.h"
#include "usb/usb_modem_probe.h"
#include "usb/usb_transfer_boot.h"
#include "reader_font.h"
#include "reader_style.h"
#include "ui/UiLocale.h"
#include "frontlight/frontlight.h"
#include "wifi/wifi_setting.h"

extern void launch_home();

static const char *TAG = "MyPaperP4";

// Temporary EPD electrical/timing diagnostic.  Keep exactly one display()
// request so repeated application refreshes cannot accumulate on the panel.
#define MYPAPER_P4_EPD_SINGLE_REFRESH_TEST 0
#define MYPAPER_P4_EPD_DATA_AAAA_HOLD_TEST 0

static void init_nvs()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

extern "C" void app_main(void)
{
    init_nvs();

    esp_err_t locale_ret = ui_locale::init();
    if (locale_ret != ESP_OK) {
        ESP_LOGW(TAG, "UI language preload failed; using English: %s",
                 esp_err_to_name(locale_ret));
    }

    // Cache the persisted network choice while the boot task still uses an
    // internal-RAM stack. Runtime network workers can live in PSRAM, but they
    // must not access NVS because flash operations disable the PSRAM cache.
    esp_err_t network_mode_ret = wifi_setting_load_network_mode();
    if (network_mode_ret != ESP_OK) {
        ESP_LOGW(TAG, "Network preference preload failed; using Wi-Fi: %s",
                 esp_err_to_name(network_mode_ret));
    }

    const bool usb_transfer_mode = usb_transfer_boot_requested();

    // The SD interface and EPD D9-D11 share the ESP32-P4 VDD_IO_5 domain.
    // Mounting SD configures on-chip LDO channel 4 to 3.3 V. Do this before
    // M5.begin(), otherwise the EPD bus starts while that domain is still at
    // its default ~1.9 V level.
    esp_err_t sd_ret = usb_transfer_mode
                           ? storage_sdcard_init_usb_medium()
                           : storage_sdcard_mount();
    if (sd_ret == ESP_OK && !usb_transfer_mode) {
        app_paths::ensure_data_dir();
    } else if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "%s failed before display init: %s",
                 usb_transfer_mode ? "SD raw-medium initialization" : "SD card mount",
                 esp_err_to_name(sd_ret));
    }

    auto cfg = M5.config();
    cfg.internal_imu = false;
    cfg.external_imu = false;
    cfg.internal_spk = false;
    cfg.internal_rtc = false;
    cfg.external_rtc = false;
    // M5Unified normally clears the display during Display.init() and once
    // again near the end of M5.begin(). Avoid those duplicate full refreshes;
    // HomeActivity will submit the first real frame once initialization ends.
    cfg.clear_display = false;
    M5.begin(cfg);

    if (!M5.Display.width() || !M5.Display.height()) {
        ESP_LOGE(TAG, "Display init failed");
        esp_restart();
    }

    // M5.begin() initializes the PCA9535/I2C bus used by the EPD. Restore
    // frontlight only afterwards so its enable line cannot race bus setup.
    frontlight_init();

    if (usb_transfer_mode) {
        touch_input_init();
        ESP_LOGI(TAG, "Starting dedicated USB MSC transfer mode");
        esp_err_t usb_ret = usb_msc_mode_run();
        ESP_LOGE(TAG, "USB MSC mode stopped unexpectedly: %s", esp_err_to_name(usb_ret));
        usb_transfer_boot_set(false);
        storage_sdcard_release_usb_medium();
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
    // ESP-Hosted is constructed before app_main(). Reserving its two large
    // streaming buffers there changes the internal-memory placement of the
    // EPD LUT and DMA buffers. Defer the reservation until the display has
    // completed those latency-sensitive allocations.
    esp_err_t hosted_rx_ret = esp_hosted_reserve_sdio_rx_buffers();
    if (hosted_rx_ret != ESP_OK) {
        ESP_LOGW(TAG, "Deferred Hosted SDIO RX reservation failed: %s",
                 esp_err_to_name(hosted_rx_ret));
    }
#endif

#if MYPAPER_P4_EPD_DATA_AAAA_HOLD_TEST
    // Static continuity/level test at the panel connector.  No EPD refresh is
    // requested and the high-voltage rails remain off.  0xAAAA means logical
    // D1,D3,...D15 are high while D0,D2,...D14 are low.
    static const gpio_num_t epd_data_pins[16] = {
        GPIO_NUM_5,  GPIO_NUM_49, GPIO_NUM_50, GPIO_NUM_28,
        GPIO_NUM_29, GPIO_NUM_30, GPIO_NUM_31, GPIO_NUM_51,
        GPIO_NUM_52, GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_46,
        GPIO_NUM_33, GPIO_NUM_32, GPIO_NUM_27, GPIO_NUM_26,
    };
    constexpr uint16_t hold_word = 0xAAAA;
    for (int bit = 0; bit < 16; ++bit) {
        gpio_set_direction(epd_data_pins[bit], GPIO_MODE_OUTPUT);
        gpio_set_level(epd_data_pins[bit], (hold_word >> bit) & 1U);
    }
    ESP_LOGW(TAG,
             "EPD DATA HOLD TEST active: D15..D0=0x%04X; "
             "odd D lines HIGH, even D lines LOW; HV/display refresh disabled",
             hold_word);
    ESP_LOGW(TAG,
             "HIGH: D1/G49 D3/G28 D5/G30 D7/G51 D9/G48 D11/G46 D13/G32 D15/G26");
    ESP_LOGW(TAG,
             "LOW : D0/G5 D2/G50 D4/G29 D6/G31 D8/G52 D10/G47 D12/G33 D14/G27");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

#if MYPAPER_P4_EPD_SINGLE_REFRESH_TEST
    ESP_LOGW(TAG, "EPD SINGLE-REFRESH TEST: queue exactly one epd_fast full-screen update; UI disabled");
    M5.Display.startWrite();
    M5.Display.setDither(false);
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.display();
    M5.Display.endWrite();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    reader_font_init();
    reader_style_init();

#if CONFIG_MYPAPER_USB_MODEM_PROBE
    // Start after M5/EPD initialization so the diagnostic's USB tasks and DMA
    // allocations cannot perturb the display engine's latency-sensitive setup.
    esp_err_t modem_probe_ret = usb_modem_probe_start();
    if (modem_probe_ret != ESP_OK) {
        ESP_LOGE(TAG, "USB modem probe start failed: %s",
                 esp_err_to_name(modem_probe_ret));
    }
#endif

    // M5.Rtc.init();
    // m5::rtc_datetime_t t = M5.Rtc.getDateTime();

    // ESP_LOGI(TAG,
    //          "Ready: %dx%d, Time: %d:%d:%d",
    //          (int)M5.Display.width(),
    //          (int)M5.Display.height(),
    //          t.time.hours,
    //          t.time.minutes,
    //          t.time.seconds);

    touch_input_init();
    launch_home();

    // In Always On mode connect without blocking touch/UI startup. The
    // on-demand default still leaves the C6 powered off until an app needs it.
    esp_err_t always_on_ret = wifi_setting_start_always_on();
    if (always_on_ret != ESP_OK) {
        ESP_LOGW(TAG, "Always-on Wi-Fi startup failed: %s", esp_err_to_name(always_on_ret));
    }

    while (true) {
        touch_input_update();

        if (ml307_voice_take_ring_hint()) {
            if (phone_activity_is_current()) phone_activity_notify_ring();
            else launch_phone(true);
        }

        if (!ActivityManager::instance().is_blocked()) {
            Activity *act = ActivityManager::instance().current();
            if (act) {
                TouchEvent event;
                while (touch_input_pop(&event)) {
                    auto &am = ActivityManager::instance();
                    act = am.current();
                    if (!act) break;

                    if (event.type == TouchEventType::Back) {
                        if (act->on_touch(event)) break;
                        if (act->allow_system_back() && am.can_pop()) {
                            am.pop();
                            break;
                        }
                        continue;
                    }

                    if (act->on_touch(event)) break;
                }

                act = ActivityManager::instance().current();
                if (!act) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                act = ActivityManager::instance().current();
                if (act) act->loop();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
