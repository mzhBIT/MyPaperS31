#include "Touch_S3207.hpp"
#include "../platforms/common.hpp"

namespace lgfx
{
 inline namespace v1
 {
  // ── RMI4: select page by writing to 0xFF ────────────────────────

  bool Touch_S3207::_selectPage(uint8_t page) {
    uint8_t buf[2] = { 0xFF, page };
    return lgfx::i2c::transactionWrite(_cfg.i2c_port, _cfg.i2c_addr, buf, 2, _cfg.freq).has_value();
  }

  // ── RMI4: read register (16-bit addr → data) ────────────────────

  bool Touch_S3207::_readRegister(uint16_t addr, uint8_t *data, uint8_t len) {
    if (!_selectPage((uint8_t)(addr >> 8))) return false;
    uint8_t ra = (uint8_t)(addr & 0xFF);
    return lgfx::i2c::transactionWriteRead(_cfg.i2c_port, _cfg.i2c_addr, &ra, 1, data, len, _cfg.freq).has_value();
  }

  bool Touch_S3207::_writeRegister(uint16_t addr, uint8_t val) {
    if (!_selectPage((uint8_t)(addr >> 8))) return false;
    uint8_t buf[2] = { (uint8_t)(addr & 0xFF), val };
    return lgfx::i2c::transactionWrite(_cfg.i2c_port, _cfg.i2c_addr, buf, 2, _cfg.freq).has_value();
  }

  // ── Touch data read ─────────────────────────────────────────────

  bool Touch_S3207::_update_data(void) {
    if (!_f12_data_base) return false;

    // Read F12_2D_DATA base to get finger count/status
    uint8_t status[2];
    if (!_readRegister(_f12_data_base, status, 2)) return false;

    uint8_t fingers = status[1] & 0x0F;
    if (fingers == 0) {
      _readdata[0] = 0;
      return true;
    }
    if (fingers > max_touch_points) fingers = max_touch_points;

    // Read finger data: each finger = 5 bytes (status, x_lo, x_hi, y_lo, y_hi)
    uint8_t fbuf[max_touch_points * 5];
    if (!_readRegister(_f12_data_base + 2, fbuf, fingers * 5)) return false;

    _readdata[0] = fingers;
    for (int i = 0; i < fingers; i++) {
      uint8_t *f = &fbuf[i * 5];
      // byte 0: status (bit 0-2=type, bit 4-7=finger_id)
      // byte 1-2: x (12bit little-endian)
      // byte 3-4: y (12bit little-endian)
      uint8_t id = (f[0] >> 4) & 0x0F;
      uint16_t x = f[1] | ((f[2] & 0x0F) << 8);
      uint16_t y = f[3] | ((f[4] & 0x0F) << 8);

      // Store: id(2) + x(2) + y(2) = 6 bytes per point, skip size
      auto *out = (uint16_t*)&_readdata[1 + i * 8];
      out[0] = id;
      out[1] = x;
      out[2] = y;
      out[3] = 0;  // size = 0
    }
    return true;
  }

  // ── Init: scan PDT to find F12 function ─────────────────────────

  bool Touch_S3207::init(void) {
    if (_inited) return true;
    if (isSPI()) return false;

    for (int retry = 3; retry; --retry) {
      if (!lgfx::i2c::init(_cfg.i2c_port, _cfg.pin_sda, _cfg.pin_scl).has_value()) {
        lgfx::delay(5); continue;
      }

      // Scan PDT pages 0..5 for function 0x12 (2D touch)
      for (int page = 0; page <= 5; page++) {
        if (!_selectPage(page)) continue;
        // PDT entries start at offset 0xE9, each entry 6 bytes
        for (int off = 0xE9; off < 0xFF; off += 6) {
          uint8_t pdt[6];
          uint8_t ra = (uint8_t)off;
          if (!lgfx::i2c::transactionWriteRead(_cfg.i2c_port, _cfg.i2c_addr, &ra, 1, pdt, 6, _cfg.freq).has_value()) break;
          uint8_t fn = pdt[5] & 0x7F;  // function number
          if (fn == 0x00) break;       // end of PDT
          if (fn == 0x12) {
            // F12 found: data_base = (page << 8) | pdt[3]
            _f12_data_base = ((uint16_t)page << 8) | pdt[3];
            // Set F12 report mode (F12_2D_CTRL)
            _writeRegister(_f12_data_base + 1, 0);  // reset
            _writeRegister(_f12_data_base, 0x03);    // enable abs + rel reports
            _inited = true;
            return true;
          }
        }
      }
      lgfx::delay(10);
    }
    return false;
  }

  void Touch_S3207::wakeup(void) {
    if (!_inited) return;
    if (_cfg.pin_rst >= 0) {
      lgfx::pinMode(_cfg.pin_rst, pin_mode_t::output);
      lgfx::gpio_lo(_cfg.pin_rst);
      lgfx::delay(1);
      lgfx::gpio_hi(_cfg.pin_rst);
      lgfx::delay(10);
    }
  }

  void Touch_S3207::sleep(void) {
    if (!_inited) return;
    _f12_data_base = 0;
  }

  uint_fast8_t Touch_S3207::getTouchRaw(touch_point_t *tp, uint_fast8_t count) {
    if (!_inited || count == 0) return 0;
    if (count > max_touch_points) count = max_touch_points;

    // Check INT pin; if asserted, read data
    if (_cfg.pin_int >= 0 && lgfx::gpio_in(_cfg.pin_int)) {
      // INT is active low for S3207; gpio_in returns true if high
      // Falling edge = touch available
      return 0;
    }

    _update_data();

    uint8_t fingers = std::min<uint8_t>((uint8_t)count, _readdata[0]);
    for (int i = 0; i < fingers; i++) {
      auto *data = (uint16_t*)&_readdata[1 + i * 8];
      tp[i].id   = data[0];
      tp[i].x    = data[1];
      tp[i].y    = data[2];
      tp[i].size = data[3];
    }
    return fingers;
  }
 }
}
