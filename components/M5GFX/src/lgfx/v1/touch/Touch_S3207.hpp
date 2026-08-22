#pragma once
#include "../Touch.hpp"

namespace lgfx
{
 inline namespace v1
 {
  struct Touch_S3207 : public ITouch
  {
    Touch_S3207(void) {
      _cfg.i2c_addr = 0x70;
      _cfg.x_min = 0;
      _cfg.x_max = 719;
      _cfg.y_min = 0;
      _cfg.y_max = 1279;
    }

    bool init(void) override;
    void wakeup(void) override;
    void sleep(void) override;
    uint_fast8_t getTouchRaw(touch_point_t *tp, uint_fast8_t count) override;

  private:
    enum { max_touch_points = 5 };
    uint8_t _readdata[max_touch_points * 4 + 2];  // 5 points × 4 bytes + status
    uint8_t _f12_data_base = 0;

    bool _readRegister(uint16_t addr, uint8_t *data, uint8_t len);
    bool _writeRegister(uint16_t addr, uint8_t val);
    bool _selectPage(uint8_t page);
    bool _update_data(void);
  };
 }
}
