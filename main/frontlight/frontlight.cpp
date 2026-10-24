#include "frontlight/frontlight.h"

#include <driver/ledc.h>
#include <esp_err.h>
#include <esp_log.h>
#include <nvs.h>

// Reuse M5GFX's already-owned I2C1 bus.  PCA9535 register 0x03 is output
// port 1 and 0x07 is its configuration port; IO1_2 is bit 2.
#include "lgfx/v1/platforms/common.hpp"

#define FRONTLIGHT_PWM_GPIO 24
#define FRONTLIGHT_PCA_PORT 1
#define FRONTLIGHT_PCA_ADDR 0x20
#define FRONTLIGHT_PCA_OUTPUT_PORT1 0x03
#define FRONTLIGHT_PCA_CONFIG_PORT1 0x07
#define FRONTLIGHT_PCA_ENABLE_BIT 0x04
#define FRONTLIGHT_NVS_NAMESPACE "frontlight"
#define FRONTLIGHT_NVS_KEY "level"

static const char *TAG = "FRONTLIGHT";
static int s_level = 5;
static bool s_initialized = false;

static int clamp_level(int level)
{
    if (level < 0) return 0;
    if (level > 10) return 10;
    return level;
}

static bool pca_read(uint8_t reg, uint8_t *value)
{
    if (!value) return false;
    auto result = lgfx::i2c::readRegister8(FRONTLIGHT_PCA_PORT, FRONTLIGHT_PCA_ADDR,
                                           reg, 100000);
    if (!result.has_value()) return false;
    *value = result.value();
    return true;
}

static bool pca_write(uint8_t reg, uint8_t value)
{
    return lgfx::i2c::writeRegister8(FRONTLIGHT_PCA_PORT, FRONTLIGHT_PCA_ADDR,
                                     reg, value, 0x00, 100000).has_value();
}

static bool set_enable(bool enabled)
{
    uint8_t config = 0;
    uint8_t output = 0;
    if (!pca_read(FRONTLIGHT_PCA_CONFIG_PORT1, &config) ||
        !pca_read(FRONTLIGHT_PCA_OUTPUT_PORT1, &output)) {
        ESP_LOGE(TAG, "PCA9535 read failed while updating enable");
        return false;
    }
    config &= ~FRONTLIGHT_PCA_ENABLE_BIT;
    if (!pca_write(FRONTLIGHT_PCA_CONFIG_PORT1, config)) {
        ESP_LOGE(TAG, "PCA9535 configure IO1_2 failed");
        return false;
    }
    if (enabled) output |= FRONTLIGHT_PCA_ENABLE_BIT;
    else output &= ~FRONTLIGHT_PCA_ENABLE_BIT;
    if (!pca_write(FRONTLIGHT_PCA_OUTPUT_PORT1, output)) {
        ESP_LOGE(TAG, "PCA9535 set IO1_2 failed");
        return false;
    }
    return true;
}

static void save_level(int level)
{
    nvs_handle_t handle = 0;
    if (nvs_open(FRONTLIGHT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    const esp_err_t result = nvs_set_u8(handle, FRONTLIGHT_NVS_KEY, (uint8_t)level);
    if (result == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

void frontlight_init(void)
{
    if (s_initialized) return;
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 50000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ledc_channel_config_t channel = {
        .gpio_num = FRONTLIGHT_PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        // The frontlight driver accepts active-low PWM: a low GPIO24 level
        // enables LED current.  Invert the LEDC output so a larger logical
        // brightness duty is physically brighter.
        .flags = { .output_invert = 1 },
    };
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO24 LEDC initialization failed");
        return;
    }
    uint8_t stored = 5;
    nvs_handle_t handle = 0;
    if (nvs_open(FRONTLIGHT_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, FRONTLIGHT_NVS_KEY, &stored);
        nvs_close(handle);
    }
    s_initialized = true;
    frontlight_set_level(clamp_level(stored));
    ESP_LOGI(TAG, "initialized level=%d (%d%%)", s_level, frontlight_get_percent());
}

int frontlight_get_level(void)
{
    return s_level;
}

int frontlight_get_color_temperature(void) { return -1; }
void frontlight_set_color_temperature(int) {}

int frontlight_get_percent(void)
{
    // Ten user steps deliberately span only the quiet 0..50% hardware PWM
    // range: each adjustment is 5% and avoids an overly bright low setting.
    return s_level * 5;
}

void frontlight_set_level(int level)
{
    level = clamp_level(level);
    if (!s_initialized) return;
    const uint32_t duty = (uint32_t)((level * 1023 + 10) / 20);
    if (level == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        set_enable(false);
    } else {
        set_enable(true);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    s_level = level;
    save_level(level);
    ESP_LOGI(TAG, "level=%d brightness=%d%% duty=%lu enable=%d", level,
             frontlight_get_percent(), (unsigned long)duty, level ? 1 : 0);
}
