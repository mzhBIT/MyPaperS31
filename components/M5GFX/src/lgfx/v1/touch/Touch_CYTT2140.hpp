#pragma once
#include "../Touch.hpp"

namespace lgfx
{
 inline namespace v1
 {
  struct Touch_CYTT2140 : public ITouch
  {
    Touch_CYTT2140(void) {
      _cfg.i2c_addr = 0x24;  // Cypress bootloader default, app mode may differ
      _cfg.x_min = 0;
      _cfg.x_max = 719;
      _cfg.y_min = 0;
      _cfg.y_max = 1279;
      _cfg.freq = 400000;
    }

    bool init(void) override;
    void wakeup(void) override;
    void sleep(void) override;
    uint_fast8_t getTouchRaw(touch_point_t *tp, uint_fast8_t count) override;

    void setFirmwarePath(const char *path) { _fw_path = path; }

  private:
    enum { max_touch_points = 5 };
    const char *_fw_path = "/spiffs/cytt2140.bin";

    bool _i2cRead(uint16_t reg, uint8_t *dst, uint8_t len);
    bool _i2cWrite(uint16_t reg, uint8_t val);
    bool _i2cWriteBuf(uint16_t reg, const uint8_t *data, uint16_t len);
    bool _loadFirmware(void);
    bool _appInit(void);
    uint8_t _readbuf[max_touch_points * 5 + 4];  // HID report buffer
  };
 }
}
