#include "S31Hardware.h"
#include "boards/MyPaperS31/ChargerState.h"
#include "boards/MyPaperS31/Pins.h"
#include <sdkconfig.h>
#if !CONFIG_IDF_TARGET_ESP32S31
#error "Independent emulator hardware is S31 only"
#endif
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace emulator {
using namespace mypaper::board::s31;
static constexpr char TAG[] = "EMU_HW";
bool S31Hardware::read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t size) {
    return dev && i2c_master_transmit_receive(dev, &reg, 1, data, size, 100) == ESP_OK;
}
bool S31Hardware::write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    const uint8_t bytes[] = {reg, value};
    return dev && i2c_master_transmit(dev, bytes, sizeof(bytes), 100) == ESP_OK;
}
bool S31Hardware::update(uint8_t port, uint8_t mask, uint8_t value) {
    if (!output_lock_ || xSemaphoreTake(output_lock_, portMAX_DELAY) != pdTRUE)
        return false;
    uint8_t old;
    const bool ok =
        read(io_, 2 + port, &old, 1) && write(io_, 2 + port, (old & ~mask) | (value & mask));
    xSemaphoreGive(output_lock_);
    return ok;
}
bool S31Hardware::init() {
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_1;
    bus.sda_io_num = GPIO_NUM_0;
    bus.scl_io_num = GPIO_NUM_1;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus, &bus_) != ESP_OK)
        return false;
    output_lock_ = xSemaphoreCreateMutex();
    if (!output_lock_)
        return false;
    auto add = [&](int address, i2c_master_dev_handle_t &device) {
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = 100000;
        return i2c_master_bus_add_device(bus_, &config, &device) == ESP_OK;
    };
    if (!add(io_address, io_) || !add(tps_address, tps_) || !add(touch_address, touch_))
        return false;
    if (add(charger_address, charger_))
        restore_charger();
    else
        ESP_LOGW(TAG, "charger bus device unavailable; charging not confirmed");
    uint8_t cfg0 = 0, cfg1 = 0;
    bool ok = false;
    for (int i = 0; i < 3 && !ok; ++i) {
        ok = read(io_, 6, &cfg0, 1) && read(io_, 7, &cfg1, 1);
        if (!ok)
            vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Preserve all unrelated outputs. Screen rail P05, touch reset P12;
    // VCOM/PWRUP/WAKE P00..02, PG P07; physical buttons P10/P11/P17.
    // Start PA P04 and BT/PA rail P14 off, without touching other latches.
    if (!ok || !update(0, 0x37, 0x20) || !update(1, 0x14, 0) ||
        !write(io_, 6, (cfg0 & ~0x37) | 0x80) || !write(io_, 7, (cfg1 & ~0x14) | 0x83))
        return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!update(1, 0x04, 0x04))
        return false;
    vTaskDelay(pdMS_TO_TICKS(50));
    // Polling touch: no interrupt pin ownership and no main application tasks.
    if (!write(touch_, 0xa4, 0))
        ESP_LOGW(TAG, "touch polling mode write failed");
    ESP_LOGI(TAG, "S31 rail/touch/keys ready; no M5 display initialization");
    return true;
}
void S31Hardware::restore_charger() {
    charger::State before{}, after{};
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        ok = charger::recover_for_boot(
            [&](uint8_t reg, uint8_t &value) { return read(charger_, reg, &value, 1); },
            [&](uint8_t reg, uint8_t value) { return write(charger_, reg, value); }, before, after);
        if (!ok)
            vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ok)
        ESP_LOGI(TAG,
                 "charger recovered: CTRL1=%02x CTRL3=%02x STATUS1=%02x; charging enabled, OTG off",
                 after.control1, after.control3, after.status);
    else
        ESP_LOGE(TAG, "charger recovery failed; charging not confirmed");
}
bool S31Hardware::power(bool on) {
    if (on == powered_)
        return true;
    if (!on) {
        gpio_set_level(gpio_num_t(epd_xoe), 0);
        gpio_set_level(gpio_num_t(epd_mode), 0);
        bool ok = update(0, 7, 7);
        vTaskDelay(pdMS_TO_TICKS(20));
        ok = update(0, 7, 4) && ok;
        vTaskDelay(pdMS_TO_TICKS(1));
        ok = update(0, 7, 0) && ok;
        powered_ = false;
        last_off_ = esp_timer_get_time();
        return ok;
    }
    const int64_t since_off = esp_timer_get_time() - last_off_;
    if (last_off_ && since_off < 250000)
        vTaskDelay(pdMS_TO_TICKS((250000 - since_off + 999) / 1000));
    bool ok = update(0, 7, 4);
    vTaskDelay(pdMS_TO_TICKS(50));
    // S31's verified VCOM is -1.23 V. Never program EEPROM/PROG/ACQ here.
    ok = ok && write(tps_, 9, 0xe4) && write(tps_, 10, 0x55) && write(tps_, 3, 123) &&
         write(tps_, 4, 0) && write(tps_, 1, 0x3f) && update(0, 7, 7);
    uint8_t pg = 0;
    const int64_t start = esp_timer_get_time();
    while (ok && esp_timer_get_time() - start < 250000) {
        if (!read(tps_, 0x0f, &pg, 1)) {
            ok = false;
            break;
        }
        if (pg == 0xfa)
            break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!ok || pg != 0xfa) {
        ESP_LOGE(TAG, "power-on failed PG=%02x", pg);
        powered_ = true;
        power(false);
        return false;
    }
    gpio_set_level(gpio_num_t(epd_mode), 1);
    powered_ = true;
    return true;
}
bool S31Hardware::input(Contact contacts[2], unsigned &count, uint8_t &port1) {
    uint8_t data[13]{};
    count = 0;
    if (!read(touch_, 2, data, sizeof(data)) || !read(io_, 1, &port1, 1))
        return false;
    count = data[0] & 15;
    if (count > 2)
        count = 0;
    for (unsigned i = 0; i < count; ++i) {
        contacts[i].x = ((data[1 + i * 6] & 15) << 8) | data[2 + i * 6];
        contacts[i].y = ((data[3 + i * 6] & 15) << 8) | data[4 + i * 6];
#if CONFIG_MYPAPER_S31_TOUCH_MIRROR_X
        contacts[i].x = 683 - contacts[i].x;
#endif
#if CONFIG_MYPAPER_S31_TOUCH_MIRROR_Y
        if (contacts[i].y < 1216)
            contacts[i].y = 1215 - contacts[i].y;
#endif
    }
    return true;
}
void S31Hardware::shutdown_outputs() {
    power(false);
}
bool S31Hardware::audio_power(bool on) {
    return update(1, 0x10, on ? 0x10 : 0);
}
bool S31Hardware::amplifier(bool on) {
    return update(0, 0x10, on ? 0x10 : 0);
}
} // namespace emulator
