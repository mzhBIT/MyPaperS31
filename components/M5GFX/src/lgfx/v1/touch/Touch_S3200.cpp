#include "Touch_S3200.hpp"
#include "../platforms/common.hpp"

namespace lgfx { inline namespace v1 {

  // ── Page select ──────────────────────────────────────────────────

  bool Touch_S3200::_selectPage(uint8_t page) {
    if (page == _page) return true;
    _page = page;
    uint8_t buf[2] = { 0xFF, page };
    return i2c::transactionWrite(_cfg.i2c_port, _cfg.i2c_addr, buf, 2, _cfg.freq).has_value();
  }

  // ── Register read/write (8-bit register within page) ────────────

  bool Touch_S3200::_readReg(uint8_t reg, uint8_t *dst, int len) {
    if (!_selectPage(reg >> 8)) return false;
    uint8_t r = (uint8_t)(reg & 0xFF);
    return i2c::transactionWriteRead(_cfg.i2c_port, _cfg.i2c_addr, &r, 1, dst, len, _cfg.freq).has_value();
  }

  bool Touch_S3200::_writeReg(uint8_t reg, uint8_t val) {
    if (!_selectPage(reg >> 8)) return false;
    uint8_t buf[2] = { (uint8_t)(reg & 0xFF), val };
    return i2c::transactionWrite(_cfg.i2c_port, _cfg.i2c_addr, buf, 2, _cfg.freq).has_value();
  }

  // ── PDT scan to find F11 (2D touch) ─────────────────────────────

  bool Touch_S3200::_scanPDT(void) {
    for (int page = 0; page < 6; page++) {
      if (!_selectPage(page)) continue;
      // PDT entries at offset 0xE9 (same as RMI4), 6 bytes each
      for (int off = 0xE9; off < 0xFF; off += 6) {
        uint8_t pdt[6];
        uint8_t r = (uint8_t)off;
        if (!i2c::transactionWriteRead(_cfg.i2c_port, _cfg.i2c_addr, &r, 1, pdt, 6, _cfg.freq).has_value())
          break;
        uint8_t fn = pdt[5] & 0x0F;  // function number
        if (fn == 0x00) break;
        // RMI data_base is at pdt[3] within the page
        if (fn == 0x11) {  // F11 = 2D touch
          _f11_data = ((uint16_t)page << 8) | pdt[3];
          _finger_support = pdt[4];  // number of fingers supported
          if (_finger_support > max_touch_points) _finger_support = max_touch_points;
          return true;
        }
      }
    }
    return false;
  }

  // ── ITouch interface ─────────────────────────────────────────────

  bool Touch_S3200::init(void) {
    if (_inited) return true;
    if (isSPI()) return false;

    if (_cfg.pin_rst >= 0) {
      lgfx::pinMode(_cfg.pin_rst, pin_mode_t::output);
      lgfx::gpio_lo(_cfg.pin_rst); lgfx::delay(5);
      lgfx::gpio_hi(_cfg.pin_rst); lgfx::delay(50);
    }

    for (int retry = 3; retry; --retry) {
      if (!lgfx::i2c::init(_cfg.i2c_port, _cfg.pin_sda, _cfg.pin_scl).has_value()) {
        lgfx::delay(10); continue;
      }
      if (_scanPDT()) {
        _inited = true;
        return true;
      }
      lgfx::delay(10);
    }
    return false;
  }

  void Touch_S3200::wakeup(void) {
    if (!_inited || _cfg.pin_rst < 0) return;
    lgfx::pinMode(_cfg.pin_rst, pin_mode_t::output);
    lgfx::gpio_lo(_cfg.pin_rst); lgfx::delay(1);
    lgfx::gpio_hi(_cfg.pin_rst); lgfx::delay(10);
  }

  void Touch_S3200::sleep(void) { /* auto-sleep */ }

  uint_fast8_t Touch_S3200::getTouchRaw(touch_point_t *tp, uint_fast8_t count) {
    if (!_inited || !_f11_data || count == 0) return 0;
    if (count > max_touch_points) count = max_touch_points;

    // INT check (active low)
    if (_cfg.pin_int >= 0 && lgfx::gpio_in(_cfg.pin_int)) return 0;

    // Read F11 data: byte[0]=status, byte[1]=finger count
    uint8_t fbuf[max_touch_points * 5 + 2];
    int rd_len = 2 + _finger_support * 5;
    if (!_readReg((uint8_t)(_f11_data & 0xFF), fbuf, rd_len)) return 0;

    uint8_t fingers = fbuf[1] & 0x0F;
    if (fingers == 0 || fingers > max_touch_points) return 0;

    uint8_t result = 0;
    for (int i = 0; i < fingers && result < (int)count; i++) {
      uint8_t *p = &fbuf[2 + i * 5];
      uint8_t st = p[0];
      if (!(st & 0x01)) continue;

      tp[result].id   = (st >> 4) & 0x0F;
      tp[result].x    = p[1] | ((p[2] & 0x0F) << 8);
      tp[result].y    = p[3] | ((p[4] & 0x0F) << 8);
      tp[result].size = 10;
      result++;
    }
    return result;
  }

} }
