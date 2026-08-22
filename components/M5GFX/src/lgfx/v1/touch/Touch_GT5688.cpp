#include "Touch_GT5688.hpp"
#include "../platforms/common.hpp"

namespace lgfx { inline namespace v1 {

// ─── I2C via lgfx::i2c (16-bit register address) ─────────────────

static bool _i2c_read16(int port, uint8_t addr, uint16_t reg,
                         uint8_t *dst, int len) {
    uint8_t wr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    auto res = i2c::transactionWriteRead(port, addr, wr, 2, dst, len, 400000);
    return res.has_value();
}

static bool _i2c_write16(int port, uint8_t addr, uint16_t reg, uint8_t val) {
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    auto res = i2c::transactionWrite(port, addr, buf, 3, 400000);
    return res.has_value();
}

// ─── Touch_GT5688 implementation ──────────────────────────────────

bool Touch_GT5688::init(void) {
    if (_inited) return true;

    // I2C bus is initialized by Bus_EPD_V7; just use it

    // Reset via RST pin
    if (_cfg.pin_rst >= 0) {
        lgfx::pinMode(_cfg.pin_rst, lgfx::pin_mode_t::output);
        lgfx::gpio_lo(_cfg.pin_rst);
        lgfx::delay(20);
        lgfx::gpio_hi(_cfg.pin_rst);
        lgfx::delay(300);
    }

    // Probe chip
    uint8_t id_buf[4];
    if (!_readRegister(0x8140, id_buf, 4)) return false;

    _inited = true;
    _flg_released = false;

    if (_cfg.pin_int >= 0) {
        lgfx::pinMode(_cfg.pin_int, lgfx::pin_mode_t::input_pullup);
    }
    return true;
}

void Touch_GT5688::wakeup(void) {
    if (!_inited) return;
    if (_cfg.pin_rst >= 0) {
        lgfx::pinMode(_cfg.pin_rst, lgfx::pin_mode_t::output);
        lgfx::gpio_lo(_cfg.pin_rst);
        lgfx::delay(1);
        lgfx::gpio_hi(_cfg.pin_rst);
    }
}

void Touch_GT5688::sleep(void) {
    // GT5688 auto-sleeps
}

uint_fast8_t Touch_GT5688::getTouchRaw(touch_point_t *tp, uint_fast8_t count) {
    if (!_inited || !tp || !count) return 0;

    if (_cfg.pin_int >= 0) {
        if (lgfx::gpio_in(_cfg.pin_int)) return 0;
    }

    // Poll header for buffer ready (up to 20ms)
    uint8_t header;
    int timeout = 20;
    do {
        if (!_readRegister(0x814E, &header, 1)) return 0;
        if (header & 0x80) break;
        lgfx::delay(1);
    } while (--timeout > 0);

    if (!(header & 0x80)) {
        _flg_released = true;
        return 0;
    }

    int touch_num = header & 0x0F;
    if (touch_num == 0) {
        _flg_released = true;
        _writeRegister(0x814E, 0);
        return 0;
    }
    if (touch_num > MAX_POINTS) touch_num = MAX_POINTS;

    // Read all contacts
    int rd_len = 1 + CONTACT_SIZE * touch_num + 1;
    uint8_t buf[1 + CONTACT_SIZE * MAX_POINTS + 1];
    if (!_readRegister(0x814E, buf, rd_len)) return 0;

    // Parse 8-byte contacts (GT1x): byte[0]=ID, [1:2]=X(LE), [3:4]=Y(LE)
    int result = 0;
    for (int i = 0; i < touch_num && result < (int)count; i++) {
        uint8_t *p = &buf[1 + i * CONTACT_SIZE];
        tp[result].x = (int)p[1] | ((int)p[2] << 8);
        tp[result].y = (int)p[3] | ((int)p[4] << 8);
        tp[result].size = 10;
        tp[result].id = p[0] & 0x0F;
        result++;
        _flg_released = false;
    }

    _writeRegister(0x814E, 0);
    return result;
}

bool Touch_GT5688::_readRegister(uint16_t reg, uint8_t *dst, int len) {
    return _i2c_read16(_cfg.i2c_port, _cfg.i2c_addr, reg, dst, len);
}

bool Touch_GT5688::_writeRegister(uint16_t reg, uint8_t val) {
    return _i2c_write16(_cfg.i2c_port, _cfg.i2c_addr, reg, val);
}

} }
