#include "StandbyS31.h"
#include "KeysS31.h"
#include "Pins.h"
#include "activities/ActivityManager.h"
#include "frontlight/frontlight.h"
#include "input/touch_input.h"
#include "time/network_time.h"
#include "ui/standby_wallpaper.h"
#include "ui/reader_book_cover.h"
#include "weread/WereadReadingSession.h"
#include "wifi/wifi_setting.h"
#include "modem/cellular_network.h"
#include "lgfx/v1/platforms/esp32/Panel_EPD_2D.hpp"
#include <M5Unified.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <mutex>
#include <cstdio>

namespace mypaper::board::s31::standby {
namespace {
constexpr const char* TAG = "S31_STANDBY";
bool pending = false;
bool shutdown_requested = false;
uint32_t requested_at = 0, generation = 0;
constexpr int held_pins[] = {sd_clk, sd_cmd, 20, 21, 22, 23, frontlight_cool, frontlight_warm};
enum class StandbyImage { None, SystemWallpaper, BookCover };

StandbyImage prepare_standby_image(const Activity &activity, bool shutting_down, int width, int height) {
    // A suspended/background reader must not affect another application's standby.
    const char *cover = shutting_down ? nullptr : activity.standby_book_cover_path();
    if (cover && cover[0] && reader_book_cover_prepare(cover, width, height, &M5.Display))
        return StandbyImage::BookCover;
    reader_book_cover_release();
    return standby_wallpaper_enabled() && standby_wallpaper_prepare(width, height)
        ? StandbyImage::SystemWallpaper : StandbyImage::None;
}

void hold_pins(bool hold) {
    for (int pin : held_pins) {
        if (hold) gpio_hold_en(static_cast<gpio_num_t>(pin));
        else gpio_hold_dis(static_cast<gpio_num_t>(pin));
    }
}
bool read_power(bool* down) {
    uint8_t ports[2];
    if (!m5::In_I2C.readRegister(io_address, 0, ports, 2, 100000)) return false;
    *down = (ports[1] & (1u << (power_key_io & 7))) == 0;
    return true;
}
bool power_pulse(bool high) {
    // Read/modify/write only P13 in output port 1; preserve touch reset,
    // Bluetooth power and every other output. Display/key I2C is idle here.
    constexpr uint8_t mask = 1u << (power_pulse_io & 7);
    return high ? m5::In_I2C.bitOn(io_address, 3, mask, 100000)
                : m5::In_I2C.bitOff(io_address, 3, mask, 100000);
}
esp_err_t cut_power() {
    constexpr uint8_t mask = 1u << (power_pulse_io & 7);
    // Set a low output latch before switching P13 to output (no spurious high).
    if (!power_pulse(false) || !m5::In_I2C.bitOff(io_address, 7, mask, 100000))
        return ESP_FAIL;
    ESP_LOGW(TAG, "hardware power-off: P13 15 pulses, high/low=100 ms");
    // IDF 6.1 Picolibc's fflush dereferences its FILE argument; fflush(NULL)
    // faults at address 0x14. Reading saves have already closed their files.
    // Flush only the two concrete console streams before pulsing the latch.
    if (stdout) std::fflush(stdout);
    if (stderr) std::fflush(stderr);
    // Normally power disappears during this sequence. Bound retries so a USB
    // powered board or broken power latch cannot get stuck forever.
    for (unsigned train = 0; train < 3; ++train) {
        for (unsigned pulse = 0; pulse < 15; ++pulse) {
            if (!power_pulse(true)) { power_pulse(false); return ESP_FAIL; }
            vTaskDelay(pdMS_TO_TICKS(100));
            if (!power_pulse(false)) { power_pulse(false); return ESP_FAIL; }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGE(TAG, "power remains on after pulse trains; returning to home");
    return ESP_ERR_TIMEOUT;
}
esp_err_t sleep_until_power() {
    const auto irq = static_cast<gpio_num_t>(io_interrupt);
    if (!esp_sleep_is_valid_wakeup_gpio(irq)) return ESP_ERR_NOT_SUPPORTED;
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << io_interrupt;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) return ret;
    // Never drive key pins high as outputs: leave P17 and all other TCA
    // inputs in their normal mode. Other shared-INT edges are filtered below.
    bool down = false;
    if (!read_power(&down) || down || gpio_get_level(irq) == 0) {
        ESP_LOGW(TAG, "sleep rejected: key/INT already low or TCA read failed");
        return ESP_ERR_INVALID_STATE;
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    for (unsigned wake = 0; wake < 32; ++wake) {
        ret = esp_sleep_enable_ext1_wakeup_io(1ULL << io_interrupt, ESP_EXT1_WAKEUP_ANY_LOW);
        if (ret != ESP_OK) break;
        hold_pins(true); // Keep SD powered/mounted; retain idle bus levels.
        std::fflush(stdout);
        ret = esp_light_sleep_start();
        hold_pins(false);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
        rtc_gpio_hold_dis(irq);
        rtc_gpio_deinit(irq);
        gpio_config(&config);
        if (ret != ESP_OK) break;
        if (!read_power(&down)) { ret = ESP_FAIL; break; }
        if (down) {
            ESP_LOGI(TAG, "power wake; same process/RAM retained");
            return ESP_OK;
        }
        // A volume/RTC/shared interrupt must not turn on the UI/backlight.
        if (gpio_get_level(irq) == 0) { ret = ESP_ERR_INVALID_STATE; break; }
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
}
}

void request() {
    if (shutdown_requested) return;
    if (pending) { pending = false; ESP_LOGI(TAG, "pending standby cancelled"); return; }
    pending = true;
    generation = ActivityManager::instance().generation();
    requested_at = lgfx::millis();
    ESP_LOGI(TAG, "standby requested; waiting for display/background I/O");
}
void request_shutdown() {
    pending = shutdown_requested = true;
    generation = ActivityManager::instance().generation();
    requested_at = lgfx::millis();
    ESP_LOGW(TAG, "shutdown requested; waiting for safe I/O boundary");
}
bool service() {
    if (!pending) return false;
    auto& manager = ActivityManager::instance();
    if (manager.generation() != generation || uint32_t(lgfx::millis()-requested_at) > 15000) {
        pending = false;
        shutdown_requested = false;
        ESP_LOGW(TAG, "power action cancelled: activity changed or background remained busy");
        return false;
    }
    if (manager.is_blocked() || M5.Display.displayBusy() || touch_input_needs_fast_poll()
        || !manager.can_standby() || wifi_setting_background_busy() || network_time_busy()) return false;
    // Blocks all live/finishing WeRead session SD/HTTP transactions, including
    // workers belonging to a previously closed book. No task is force-suspended.
    std::unique_lock<std::mutex> reports(weread::reading_transaction_mutex(), std::try_to_lock);
    if (!reports.owns_lock()) return false;
    auto* activity = manager.current();
    if (!activity) { pending = false; return false; }
    if (!network_time_try_pause()) return false;
    struct TimeResume { ~TimeResume() { network_time_resume(); } } time_resume;
    const bool shutting_down = shutdown_requested;
    pending = shutdown_requested = false;
    if (!activity->prepare_standby()) {
        ESP_LOGW(TAG, "power action cancelled: application I/O could not be stopped safely");
        return true; // Keep Wi-Fi/netif alive for the unfinished application.
    }
    activity->on_standby_enter(); // Pause reading clocks; do not count sleep.
    if (!keys::pause_for_sleep()) {
        activity->on_standby_resume();
        ESP_LOGW(TAG, "standby cancelled: key worker did not become idle");
        return true;
    }
    if (!touch_input_pause_for_sleep()) {
        keys::resume_after_sleep();
        activity->on_standby_resume();
        ESP_LOGW(TAG, "standby cancelled: touch sampler did not become idle");
        return true;
    }
    struct TouchResume {
        ~TouchResume() { touch_input_resume_after_sleep(); }
    } touch_resume;
    const bool wifi_enabled = wifi_setting_is_enabled();
    const bool wifi_connected = wifi_setting_is_connected();
    // Join NT26 tasks and release UHCI/PM locks before light sleep. P14 stays
    // high because it is shared with audio; modem GPIOs return to idle.
    esp_err_t ret = cellular_network_suspend();
    if (ret == ESP_OK && wifi_enabled) ret = wifi_setting_deinit(false);
    using Buffer = std::unique_ptr<uint8_t, decltype(&heap_caps_free)>;
    Buffer snapshot(nullptr, heap_caps_free);
    auto* panel = static_cast<lgfx::Panel_EPD_2D*>(M5.Display.panel());
    constexpr size_t bytes = 1216u * 684u / 2;
    const auto old_mode = M5.Display.getEpdMode();
    const bool old_dither = M5.Display.getDither();
    bool wallpaper_shown = false;
    const auto image = ret == ESP_OK
        ? prepare_standby_image(*activity, shutting_down, M5.Display.width(), M5.Display.height())
        : StandbyImage::None;
    if (image != StandbyImage::None) {
        snapshot.reset(static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (snapshot && panel->copyPhysicalGrayscale4(snapshot.get(), bytes)) {
            M5.Display.startWrite();
            M5.Display.setEpdMode(lgfx::epd_mode_t::epd_quality);
            M5.Display.setDither(false);
            wallpaper_shown = image == StandbyImage::BookCover
                ? reader_book_cover_draw(M5.Display, M5.Display.width(), M5.Display.height())
                : standby_wallpaper_draw(M5.Display, M5.Display.width(), M5.Display.height());
            if (wallpaper_shown) M5.Display.display();
            M5.Display.endWrite();
            M5.Display.waitDisplay();
        } else {
            snapshot.reset();
            ESP_LOGW(TAG, "wallpaper skipped: page snapshot unavailable");
        }
    }
    reader_book_cover_release();
    standby_wallpaper_release();
    if (ret == ESP_OK && shutting_down) {
        frontlight_set_suspended(true);
        snapshot.reset(); // No return to the old activity after closing apps.
        manager.close_apps_for_shutdown(); // Save local progress and reading time.
        // Reader destruction publishes its final WeRead mailbox. Let those
        // workers persist it offline before removing power; never wait with
        // their transaction mutex held and never start a final network report.
        reports.unlock();
        const uint32_t start = lgfx::millis();
        while (!weread::reading_sessions_idle() && uint32_t(lgfx::millis()-start) < 10000)
            vTaskDelay(pdMS_TO_TICKS(20));
        M5.Display.waitDisplay();
        if (weread::reading_sessions_idle()) ret = cut_power();
        else { ret = ESP_ERR_TIMEOUT; ESP_LOGE(TAG, "shutdown cancelled: pending reading saves"); }
        // Reaching here means the supply stayed on or an I2C/save error occurred.
        // Keep the device usable; no misleading reboot or fake shutdown.
        M5.Display.setEpdMode(old_mode);
        M5.Display.setDither(old_dither);
        frontlight_set_suspended(false);
        touch_input_cancel_until_release();
        keys::resume_after_sleep();
        cellular_network_resume();
        if (manager.current()) manager.current()->on_show();
        if (wifi_connected || wifi_setting_get_power_mode() == WIFI_SETTING_POWER_ALWAYS_ON)
            wifi_setting_resume_saved_async();
        else if (wifi_enabled) wifi_setting_init();
        ESP_LOGE(TAG, "shutdown returned: %s", esp_err_to_name(ret));
        return true;
    }
    if (ret == ESP_OK) {
        frontlight_set_suspended(true); // No NVS write and no user-level change.
        ESP_LOGI(TAG, "enter light sleep wallpaper=%d source=%s frontlight=off", wallpaper_shown,
            wallpaper_shown ? (image == StandbyImage::BookCover ? "book-cover" : "system") : "none");
        ret = sleep_until_power();
    }
    if (snapshot) {
        M5.Display.startWrite();
        M5.Display.setEpdMode(lgfx::epd_mode_t::epd_quality);
        M5.Display.setDither(false);
        panel->writePhysicalGrayscale4(snapshot.get(), 1216, 684, M5.Display.getRotation());
        M5.Display.display();
        M5.Display.endWrite();
        M5.Display.waitDisplay();
    }
    M5.Display.setEpdMode(old_mode);
    M5.Display.setDither(old_dither);
    frontlight_set_suspended(false);
    touch_input_cancel_until_release();
    keys::resume_after_sleep(); // Swallow the wake key until its release.
    cellular_network_resume();
    activity->on_standby_resume();
    if (wifi_connected || wifi_setting_get_power_mode() == WIFI_SETTING_POWER_ALWAYS_ON)
        wifi_setting_resume_saved_async();
    else if (wifi_enabled) wifi_setting_init();
    ESP_LOGI(TAG, "standby returned: %s; previous page/brightness restored", esp_err_to_name(ret));
    return true;
}
}
