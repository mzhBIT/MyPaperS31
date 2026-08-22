#pragma once

#include "../Touch.hpp"

namespace lgfx { inline namespace v1 {

// NS2009 I2C resistive touch controller (Nsiway)
// I2C addr 0x48 (7-bit), 12-bit SAR ADC
// PENIRQ active-low, wired to PCA9535 IO0_7 on MyPaperS3
  struct Touch_NS2009 : public ITouch
  {
    Touch_NS2009(void) {
      _cfg.i2c_addr = 0x48;
      _cfg.x_min = 0; _cfg.x_max = 4095;   // 12-bit ADC raw
      _cfg.y_min = 0; _cfg.y_max = 4095;
      _cfg.freq  = 400000;                  // 400 kHz I2C
    }

    bool init(void) override;
    void wakeup(void) override;
    void sleep(void) override;
    uint_fast8_t getTouchRaw(touch_point_t *tp, uint_fast8_t count) override;

    // PENIRQ wired to PCA9535 IO0_7 instead of a direct GPIO.
    // port: I2C port (I2C_NUM_1 on MyPaperS3), addr: PCA9535 addr (0x20)
    void setPCA9535IRQ(int i2c_port, uint8_t i2c_addr) {
      _pca_port = i2c_port; _pca_addr = i2c_addr;
    }

  private:
    int     _pca_port = -1;     // -1 → use pin_int direct GPIO
    uint8_t _pca_addr = 0x20;   // PCA9535 I2C address

    static constexpr uint8_t CMD_X  = 0xC0;  // read X, 12-bit, low-power
    static constexpr uint8_t CMD_Y  = 0xD0;  // read Y, 12-bit, low-power
    static constexpr uint8_t CMD_Z1 = 0xE0;  // read Z1, 12-bit, low-power
    static constexpr uint8_t CMD_Z2 = 0xF0;  // read Z2, 12-bit, low-power

    uint16_t _readChannel(uint8_t cmd);
    bool     _isTouched(void);
  };

} }
