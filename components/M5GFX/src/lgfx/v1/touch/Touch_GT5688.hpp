/*----------------------------------------------------------------------------/
  Lovyan GFX — Touch driver for Goodix GT5688 (GT1x series, 8-byte contacts).

  I2C address: 0x5D
  16-bit register addressing (big-endian), up to 10 touch points.
/----------------------------------------------------------------------------*/
#pragma once

#include "../Touch.hpp"

namespace lgfx { inline namespace v1 {

  struct Touch_GT5688 : public ITouch
  {
    static constexpr const uint8_t GT5688_ADDR = 0x5D;

    Touch_GT5688(void) {
      _cfg.i2c_addr = GT5688_ADDR;
      _cfg.x_min = 0;
      _cfg.x_max = 4095;
      _cfg.y_min = 0;
      _cfg.y_max = 4095;
    }

    bool init(void) override;
    void wakeup(void) override;
    void sleep(void) override;
    uint_fast8_t getTouchRaw(touch_point_t *tp, uint_fast8_t count) override;

  private:
    enum { MAX_POINTS = 10, CONTACT_SIZE = 8 };

    bool _readRegister(uint16_t reg, uint8_t *dst, int len);
    bool _writeRegister(uint16_t reg, uint8_t val);

    uint32_t _last_update = 0;
    uint32_t _refresh_rate = 5;
    bool _flg_released = false;
  };

} }
