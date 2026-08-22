#pragma once
#include "../Touch.hpp"

namespace lgfx { inline namespace v1 {
  struct Touch_S3200 : public ITouch
  {
    Touch_S3200(void) {
      _cfg.i2c_addr = 0x20;
      _cfg.x_min = 0; _cfg.x_max = 1080 - 1;
      _cfg.y_min = 0; _cfg.y_max = 1920 - 1;
      _cfg.freq = 400000;
    }

    bool init(void) override;
    void wakeup(void) override;
    void sleep(void) override;
    uint_fast8_t getTouchRaw(touch_point_t *tp, uint_fast8_t count) override;

  private:
    enum { max_touch_points = 5 };
    uint8_t _readbuf[max_touch_points * 5 + 4];
    uint16_t _f11_data = 0;    // F11 touch data base addr
    uint8_t _finger_support = 5;
    uint8_t _page = 0;

    bool _selectPage(uint8_t page);
    bool _readReg(uint8_t reg, uint8_t *dst, int len);
    bool _writeReg(uint8_t reg, uint8_t val);
    bool _scanPDT(void);
  };
} }
