#include "frontlight/frontlight.h"
#include "frontlight/FrontlightMix.h"
#include "boards/MyPaperS31/Pins.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <nvs.h>
#include <sdkconfig.h>
#if !CONFIG_IDF_TARGET_ESP32S31
#error "S31 frontlight compiled for the wrong board"
#endif

namespace {
constexpr char TAG[] = "S31_FRONTLIGHT";
constexpr char NVS_NS[] = "frontlight_s31";
constexpr auto MODE = LEDC_LOW_SPEED_MODE;
bool initialized = false;
int level = 0, temperature = 5;
frontlight_mix::Duty active{};

bool apply(int next_level, int next_temperature) {
    const auto next = frontlight_mix::mix(next_level, next_temperature);
    auto write = [](ledc_channel_t ch, uint32_t duty) {
        // Immediate PWM only. The combined set_duty_and_update API uses the
        // fade service's per-channel locks in IDF 6.1, even without a fade.
        // This driver is controlled serially by app_main, so no fade service
        // or extra allocation is required (same API pair as the reference).
        return ledc_set_duty(MODE, ch, duty) == ESP_OK
            && ledc_update_duty(MODE, ch) == ESP_OK;
    };
    // Lower departing channel(s) first to avoid a transient double-bright blend.
    bool ok = true;
    if (next.cool < active.cool) ok = write(LEDC_CHANNEL_0, next.cool) && ok;
    if (next.warm < active.warm) ok = write(LEDC_CHANNEL_1, next.warm) && ok;
    if (next.cool >= active.cool) ok = write(LEDC_CHANNEL_0, next.cool) && ok;
    if (next.warm >= active.warm) ok = write(LEDC_CHANNEL_1, next.warm) && ok;
    if (!ok) {
        write(LEDC_CHANNEL_0, 0); write(LEDC_CHANNEL_1, 0);
        active = {};
        level = 0;
        ESP_LOGE(TAG, "PWM update failed; requested both channels off");
        return false;
    }
    active = next;
    return true;
}
void save() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "level", level);
        if (err == ESP_OK) err = nvs_set_u8(h, "cct", temperature);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "settings save failed: %s", esp_err_to_name(err));
}
}

void frontlight_init(void) {
    if (initialized) return;
    using namespace mypaper::board::s31;
    for (int pin : {frontlight_cool, frontlight_warm}) {
        gpio_hold_dis(static_cast<gpio_num_t>(pin));
        gpio_reset_pin(static_cast<gpio_num_t>(pin));
        gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    }
    ledc_timer_config_t timer{};
    timer.speed_mode = MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 20000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGE(TAG, "20kHz PWM timer initialization failed"); return;
    }
    ledc_channel_config_t ch{};
    ch.speed_mode = MODE;
    ch.timer_sel = LEDC_TIMER_0;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.gpio_num = frontlight_cool;
    ch.channel = LEDC_CHANNEL_0;
    // Both S31 channels are active high (not the P4 output-inverted circuit).
    esp_err_t err = ledc_channel_config(&ch);
    if (err == ESP_OK) {
        ch.gpio_num = frontlight_warm; ch.channel = LEDC_CHANNEL_1;
        err = ledc_channel_config(&ch);
    }
    if (err != ESP_OK) {
        ledc_stop(MODE, LEDC_CHANNEL_0, 0);
        ledc_stop(MODE, LEDC_CHANNEL_1, 0);
        ESP_LOGE(TAG, "PWM channel initialization failed: %s", esp_err_to_name(err));
        return;
    }
    uint8_t stored_level = 0, stored_temperature = 5;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "level", &stored_level);
        nvs_get_u8(h, "cct", &stored_temperature);
        nvs_close(h);
    }
    temperature = stored_temperature <= 10 ? stored_temperature : 5;
    const int restored_level = stored_level <= 10 ? stored_level : 0;
    initialized = true;
    if (apply(restored_level, temperature)) level = restored_level;
    ESP_LOGI(TAG, "ready cool=GPIO%d warm=GPIO%d active_high=1 PWM=20000Hz level=%d cct=%d",
             frontlight_cool, frontlight_warm, level, temperature);
}
int frontlight_get_level(void) { return level; }
int frontlight_get_percent(void) { return level * 5; }
int frontlight_get_color_temperature(void) { return temperature; }
void frontlight_set_level(int value) {
    value = frontlight_mix::clamp(value);
    if (!initialized || value == level || !apply(value, temperature)) return;
    level = value; save();
    ESP_LOGI(TAG, "brightness=%d%% cct=%d cool_duty=%lu warm_duty=%lu",
             frontlight_get_percent(), temperature, (unsigned long)active.cool, (unsigned long)active.warm);
}
void frontlight_set_color_temperature(int value) {
    value = frontlight_mix::clamp(value);
    if (!initialized || value == temperature || !apply(level, value)) return;
    temperature = value; save();
    ESP_LOGI(TAG, "brightness=%d%% cct=%d cool_duty=%lu warm_duty=%lu",
             frontlight_get_percent(), temperature, (unsigned long)active.cool, (unsigned long)active.warm);
}
