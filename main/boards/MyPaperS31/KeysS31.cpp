#include "KeysS31.h"
#include "Pins.h"
#include "StandbyS31.h"
#include "input/BoardKeyState.h"
#include "audio/audio_radio.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include <M5Unified.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <algorithm>

namespace mypaper::board::s31::keys {
namespace {
using board_keys::Key;
using board_keys::Gesture;
constexpr const char* TAG = "S31_KEYS";
struct Event { Key key; Gesture gesture; uint32_t at, context; };
QueueHandle_t queue = nullptr;
TaskHandle_t task = nullptr;
std::atomic<uint32_t> context{0};
std::atomic<bool> pause_requested{false}, pause_ack{false};
board_keys::Cover cover;
bool cover_pending = false; // Main-thread producer/consumer only.

const char* name(Key key) {
    switch (key) {
    case Key::Boot: return "BOOT";
    case Key::VolumeUp: return "VOL+";
    case Key::VolumeDown: return "VOL-";
    case Key::Power: return "POWER";
    case Key::Home: return "HOME";
    case Key::Previous: return "PREVIOUS";
    case Key::Next: return "NEXT";
    default: return "NONE";
    }
}
void emit(Key key, Gesture gesture, uint32_t now) {
    if (!queue || gesture == Gesture::None) return;
    if (cover_pending) return;
    const Event event{key, gesture, now, context.load()};
    if (xQueueSend(queue, &event, 0) != pdTRUE)
        ESP_LOGW(TAG, "key queue full; dropped %s", name(key));
    else cover_pending = true;
}
bool read_keys(uint8_t* port1) {
    // Read both input ports in one transaction; preserve all output latches.
    // Polling avoids claiming GPIO2, shared with RTC/accelerometer interrupts.
    uint8_t ports[2];
    if (!m5::In_I2C.readRegister(io_address, 0, ports, 2, 100000)) return false;
    *port1 = ports[1];
    return true;
}
void scan(void*) {
    constexpr Key ids[] = {Key::Boot, Key::VolumeUp, Key::VolumeDown, Key::Power};
    constexpr uint8_t masks[] = {0, uint8_t(1u << (volume_up_io & 7)),
        uint8_t(1u << (volume_down_io & 7)), uint8_t(1u << (power_key_io & 7))};
    board_keys::Button buttons[4];
    bool initialized[4]{};
    bool read_failed = false;
    uint32_t generation = context.load();
    for (;;) {
        if (pause_requested.load()) {
            pause_ack.store(true); // Previous I2C transaction has completed.
            while (pause_requested.load()) vTaskDelay(pdMS_TO_TICKS(10));
            pause_ack.store(false);
            for (auto& value : initialized) value = false;
            generation = context.load();
            continue;
        }
        const uint32_t now = lgfx::millis();
        const auto current = context.load();
        if (current != generation) {
            for (auto& button : buttons) button.cancel_held();
            generation = current;
        }
        uint8_t port1 = 0xff;
        const bool ok = read_keys(&port1);
        if (ok == read_failed) {
            ESP_LOGW(TAG, "TCA key read %s", ok ? "recovered" : "failed; no synthetic release");
            read_failed = !ok;
        }
        for (unsigned i=0; i<4; ++i) {
            if (i && !ok) continue;
            const bool down = i ? !(port1 & masks[i])
                                : gpio_get_level(static_cast<gpio_num_t>(boot_button)) == 0;
            if (!initialized[i]) { buttons[i].reset(down, now); initialized[i] = true; continue; }
            const Gesture gesture = buttons[i].sample(down, now, i == 3 ? 3000 : 500);
            // Capture context from the scan, not after a racing main-thread
            // screen switch. The consumer discards old-context events.
            if (gesture != Gesture::None) {
                const Event event{ids[i], gesture, now, generation};
                if (xQueueSend(queue, &event, 0) != pdTRUE)
                    ESP_LOGW(TAG, "key queue full; dropped %s", name(ids[i]));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void back(Activity* activity) {
    auto& manager = ActivityManager::instance();
    if (activity->on_key(APP_KEY_BACK)) return;
    TouchEvent event; event.type = TouchEventType::Back;
    if (activity->on_touch(event)) return;
    if (activity->allow_system_back() && manager.can_pop()) manager.pop();
}
}

void init() {
    if (task) return;
    gpio_config_t gpio{};
    gpio.pin_bit_mask = 1ULL << boot_button;
    gpio.mode = GPIO_MODE_INPUT;
    gpio.pull_up_en = GPIO_PULLUP_ENABLE;
    if (gpio_config(&gpio) != ESP_OK) return;
    // P10/P11/P17 input. Do not touch P12 touch reset, P13 shutdown,
    // P14 BT power, P16 RTC or any port-0 rail/control output.
    uint8_t config;
    if (!m5::In_I2C.readRegister(io_address, 7, &config, 1, 100000)
        || !m5::In_I2C.writeRegister8(io_address, 7, config | 0x83, 100000)) {
        ESP_LOGW(TAG, "TCA key setup failed; cover keys still available");
        // BOOT is independent; worker will retry input reads without writes.
    }
    if (!queue) queue = xQueueCreate(16, sizeof(Event));
    if (!queue) { ESP_LOGW(TAG, "key queue allocation failed"); return; }
    if (xTaskCreatePinnedToCore(scan, "s31_keys", 3072, nullptr, 2, &task, 0) != pdPASS) {
        task = nullptr;
        ESP_LOGW(TAG, "physical key worker allocation failed; cover keys remain available");
        return;
    }
    ESP_LOGI(TAG, "ready BOOT=GPIO61 VOL=P10/P11 POWER=P17 poll=20ms debounce=30ms; cover raw Y=1600");
}
void sync_context(uint32_t generation) {
    if (generation == context.load()) return;
    context.store(generation);
    cover.cancel();
    cover_pending = false;
    if (queue) xQueueReset(queue);
}
bool cover_sample(bool pressed, int raw_x, int raw_y, uint32_t now) {
    const Key hit = pressed ? board_keys::cover_hit(raw_x, raw_y) : Key::None;
    const bool outside = pressed && (raw_x < 0 || raw_x > 683 || raw_y < 0 || raw_y > 1215);
    if (!cover.active && !outside && hit == Key::None) return false;
    const bool was_active = cover.active;
    const auto gesture = cover.sample(pressed, hit, now,
        !cover_pending && !M5.Display.displayBusy() && !ActivityManager::instance().is_blocked());
    emit(cover.key, gesture, now);
    return was_active || cover.active;
}
bool cover_active() { return cover.active; }

bool pause_for_sleep() {
    cover.cancel();
    if (!task) return true;
    pause_requested.store(true);
    const uint32_t start = lgfx::millis();
    while (!pause_ack.load() && uint32_t(lgfx::millis()-start) < 1000)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!pause_ack.load()) { pause_requested.store(false); return false; }
    return true;
}
void resume_after_sleep() {
    if (queue) xQueueReset(queue);
    cover.cancel(); cover_pending = false;
    pause_requested.store(false);
}

bool dispatch_one() {
    auto& manager = ActivityManager::instance();
    sync_context(manager.generation());
    if (!queue || manager.is_blocked() || M5.Display.displayBusy()) return false;
    Event event;
    while (xQueueReceive(queue, &event, 0) == pdTRUE) {
        if (event.key == Key::Home || event.key == Key::Previous || event.key == Key::Next)
            cover_pending = false;
        if (event.context != context.load() || uint32_t(lgfx::millis()-event.at) > 1500) continue;
        auto* activity = manager.current();
        if (!activity) return false;
        ESP_LOGI(TAG, "%s %s", name(event.key), event.gesture == Gesture::Click ? "click"
                 : event.gesture == Gesture::Repeat ? "repeat" : "long");
        switch (event.key) {
        case Key::Boot:
            if (event.gesture == Gesture::Click) activity->on_reader_menu_key();
            // AI assistant is not implemented. Do not reinterpret long press.
            break;
        case Key::Home:
            if (event.gesture == Gesture::Click && activity->on_cover_home_key()) break;
            if (activity->allow_system_home() && manager.can_pop()) manager.pop_to_root();
            break;
        case Key::Previous:
        case Key::Next:
            if (!activity->on_navigation_key(event.key == Key::Previous ? -1 : 1)
                && event.key == Key::Previous && event.gesture != Gesture::Repeat) back(activity);
            break;
        case Key::Power:
            manager.dismiss_overlay(true);
            if (event.gesture == Gesture::Click) standby::request();
            else if (event.gesture == Gesture::LongPress) standby::request_shutdown();
            break;
        case Key::VolumeUp: case Key::VolumeDown: {
            const int delta = board_keys::volume_delta(event.key,event.gesture);
            if (!delta || activity->on_volume_key(delta)) break;
            const int current = audio_radio_get_volume();
            const int volume = std::clamp(current+delta,0,100);
            if (volume != current) {
                const esp_err_t result = audio_radio_set_volume(volume);
                if (result != ESP_OK) ESP_LOGW(TAG, "volume adjustment failed: %s", esp_err_to_name(result));
            }
            break;
        }
        default: break;
        }
        sync_context(manager.generation());
        return true;
    }
    return false;
}
}
