#pragma once
#include <cstddef>
#include <cstdint>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace emulator {
struct Contact {
    int x = 0, y = 0;
};
class S31Hardware {
  public:
    bool init();
    bool power(bool on);
    bool input(Contact contacts[2], unsigned &count, uint8_t &port1);
    void shutdown_outputs();
    bool enable_audio_power();
    bool amplifier(bool on);

  private:
    bool read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t size);
    bool write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value);
    bool update(uint8_t port, uint8_t mask, uint8_t value);
    void restore_charger();
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t io_ = nullptr, tps_ = nullptr, touch_ = nullptr;
    i2c_master_dev_handle_t charger_ = nullptr;
    bool powered_ = false;
    SemaphoreHandle_t output_lock_ = nullptr;
    int64_t last_off_ = 0;
};
} // namespace emulator
