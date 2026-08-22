#include "Touch_NS2009.hpp"
#include "../platforms/common.hpp"

namespace lgfx { inline namespace v1 {

  // ─── Read one ADC channel ──────────────────────────────────────────

  uint16_t Touch_NS2009::_readChannel(uint8_t cmd) {
    uint8_t wr = cmd;
    uint8_t rd[2] = { 0 };
    if (!i2c::transactionWriteRead(_cfg.i2c_port, _cfg.i2c_addr,
                                   &wr, 1, rd, 2, _cfg.freq).has_value()) {
      return 0xFFF;  // I2C error → return max (floating / not touched)
    }
    // 12-bit data left-aligned in 16 bits → shift right 4
    uint16_t v = ((uint16_t)rd[0] << 8) | rd[1];
    return v >> 4;
  }

  // ─── Touch detection (PENIRQ) ──────────────────────────────────────

  bool Touch_NS2009::_isTouched(void) {
    // Priority 1: PCA9535 IO0_7 (low = touched)
    if (_pca_port >= 0) {
      auto r = i2c::readRegister8(_pca_port, _pca_addr, 0x00, 100000);
      if (r.has_value()) {
        return !(r.value() & 0x80);  // IO0_7 = 0 → pressed
      }
      return false;
    }
    // Priority 2: direct GPIO (active low)
    if (_cfg.pin_int >= 0) {
      return !lgfx::gpio_in(_cfg.pin_int);
    }
    // No INT source → always try reading (0xFFF = not touched)
    return true;
  }

  // ─── ITouch interface ──────────────────────────────────────────────

  bool Touch_NS2009::init(void) {
    if (_inited) return true;
    if (isSPI()) return false;

    if (!lgfx::i2c::init(_cfg.i2c_port, _cfg.pin_sda, _cfg.pin_scl).has_value())
      return false;

    // Probe: try reading X channel — device must ACK
    uint8_t wr = CMD_X;
    uint8_t rd[2];
    if (!i2c::transactionWriteRead(_cfg.i2c_port, _cfg.i2c_addr,
                                   &wr, 1, rd, 2, _cfg.freq).has_value())
      return false;

    // PCA9535 I2C: reuse already-initialized bus
    if (_pca_port >= 0) {
      lgfx::i2c::init(_pca_port, _cfg.pin_sda, _cfg.pin_scl);
    }

    _inited = true;
    return true;
  }

  void Touch_NS2009::wakeup(void) {
    // NS2009 auto-wakes on any I2C transaction
  }

  void Touch_NS2009::sleep(void) {
    // Low-power mode: PD0=0 auto-powers-down ADC between reads
  }

  uint_fast8_t Touch_NS2009::getTouchRaw(touch_point_t *tp, uint_fast8_t count) {
    if (!_inited || count == 0) return 0;

    // Check PENIRQ
    if (!_isTouched()) return 0;

    // Read X and Y; a floating panel returns 0xFFF (4095)
    uint16_t x = _readChannel(CMD_X);
    uint16_t y = _readChannel(CMD_Y);

    // Ignore if readings are near max (pen up / noise)
    if (x >= 0xFF0 || y >= 0xFF0) return 0;

    tp[0].x    = (int16_t)x;
    tp[0].y    = (int16_t)y;
    tp[0].size = 10;
    tp[0].id   = 0;
    return 1;  // single-touch resistive panel
  }

} }
