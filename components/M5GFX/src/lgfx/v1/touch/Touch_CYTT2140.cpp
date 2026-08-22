#include "Touch_CYTT2140.hpp"
#include "../platforms/common.hpp"
#include <string.h>

namespace lgfx { inline namespace v1 {

// ── I2C helpers ───────────────────────────────────────────────────

bool Touch_CYTT2140::_i2cRead(uint16_t reg, uint8_t *dst, uint8_t len) {
    uint8_t wr[2] = { (uint8_t)(reg & 0xFF), (uint8_t)(reg >> 8) };
    return i2c::transactionWriteRead(_cfg.i2c_port, _cfg.i2c_addr, wr, 2, dst, len, _cfg.freq).has_value();
}

bool Touch_CYTT2140::_i2cWrite(uint16_t reg, uint8_t val) {
    uint8_t buf[3] = { (uint8_t)(reg & 0xFF), (uint8_t)(reg >> 8), val };
    return i2c::transactionWrite(_cfg.i2c_port, _cfg.i2c_addr, buf, 3, _cfg.freq).has_value();
}

bool Touch_CYTT2140::_i2cWriteBuf(uint16_t reg, const uint8_t *data, uint16_t len) {
    // Allocate temp buffer: reg(2) + data(len)
    uint16_t total = 2 + len;
    auto *buf = (uint8_t *)alloca(total);
    buf[0] = (uint8_t)(reg & 0xFF);
    buf[1] = (uint8_t)(reg >> 8);
    if (len) memcpy(buf + 2, data, len);
    return i2c::transactionWrite(_cfg.i2c_port, _cfg.i2c_addr, buf, total, _cfg.freq).has_value();
}

// ── Cypress Bootloader ────────────────────────────────────────────

// Security key required before FW programming
static const uint8_t _sec_key[] = { 0xA5, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0x5A };

bool Touch_CYTT2140::_loadFirmware(void) {
    // Open firmware from SPIFFS
    FILE *fp = fopen(_fw_path, "rb");
    if (!fp) {
        // No firmware file — chip may already be programmed
        return false;
    }
    fseek(fp, 0, SEEK_END);
    size_t fw_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fw_size == 0 || fw_size > 128 * 1024) { fclose(fp); return false; }

    // Enter bootloader: reset + I2C access at bootloader address
    if (_cfg.pin_rst >= 0) {
        lgfx::pinMode(_cfg.pin_rst, pin_mode_t::output);
        lgfx::gpio_lo(_cfg.pin_rst); lgfx::delay(5);
        lgfx::gpio_hi(_cfg.pin_rst); lgfx::delay(50);
    } else {
        // Soft reset via register
        _i2cWrite(0xFF, 0x01);  // CMD_RESET
        lgfx::delay(100);
    }

    // Write security key to unlock programming
    uint8_t status;
    for (int retry = 5; retry; --retry) {
        if (_i2cWriteBuf(0x01, _sec_key, 8)) break;
        lgfx::delay(50);
    }

    // Write firmware rows (128 bytes each, 16-bit row address)
    uint8_t row[128 + 3];   // cmd + row_num + row_size + data
    row[0] = 0x04;          // CMD_PROGRAM_AND_VERIFY
    size_t offset = 0;
    uint16_t row_num = 0;

    while (offset < fw_size) {
        size_t chunk = (fw_size - offset > 128) ? 128 : (fw_size - offset);
        row[1] = (uint8_t)(row_num & 0xFF);
        row[2] = (uint8_t)(row_num >> 8);
        fread(&row[3], 1, chunk, fp);

        if (!_i2cWriteBuf(0x01, row, 3 + chunk)) {
            fclose(fp);
            return false;
        }

        // Poll status
        int timeout = 20;
        do {
            _i2cRead(0x00, &status, 1);
            if (status == 0x00) break;
            lgfx::delay(2);
        } while (--timeout);

        offset += chunk;
        row_num++;
    }
    fclose(fp);

    // Exit bootloader → application mode
    _i2cWrite(0x01, 0x02);  // CMD_EXIT
    lgfx::delay(100);
    return true;
}

// ── Application init (HID descriptor discovery) ──────────────────

bool Touch_CYTT2140::_appInit(void) {
    // After FW load, chip is at app mode I2C address
    // Read HID descriptor to find touch report register
    _cfg.i2c_addr = 0x24;  // app mode address (same as bootloader for this chip)
    lgfx::delay(50);

    // Probe: try to read HID descriptor length
    uint8_t buf[4];
    if (!_i2cRead(0x0001, buf, 4)) {
        // Try alternate address
        _cfg.i2c_addr = 0x25;
        if (!_i2cRead(0x0001, buf, 4)) return false;
    }
    return true;
}

// ── Touch_ITouch interface ────────────────────────────────────────

bool Touch_CYTT2140::init(void) {
    if (_inited) return true;
    if (isSPI()) return false;

    // Init I2C bus
    if (!i2c::init(_cfg.i2c_port, _cfg.pin_sda, _cfg.pin_scl).has_value())
        return false;

    // Load firmware if available
    // _loadFirmware();

    // Init app mode
    if (!_appInit()) return false;

    _inited = true;
    return true;
}

void Touch_CYTT2140::wakeup(void) {
    if (!_inited) return;
    if (_cfg.pin_rst >= 0) {
        lgfx::pinMode(_cfg.pin_rst, pin_mode_t::output);
        lgfx::gpio_lo(_cfg.pin_rst);
        lgfx::delay(1);
        lgfx::gpio_hi(_cfg.pin_rst);
        lgfx::delay(10);
    }
}

void Touch_CYTT2140::sleep(void) {
    // chip auto-sleeps
}

uint_fast8_t Touch_CYTT2140::getTouchRaw(touch_point_t *tp, uint_fast8_t count) {
    if (!_inited || count == 0) return 0;
    if (count > max_touch_points) count = max_touch_points;

    // INT check: active low
    if (_cfg.pin_int >= 0 && lgfx::gpio_in(_cfg.pin_int)) return 0;

    // Read HID report from register 0x0000 (input report)
    // Format: byte[0]=report_id(0x01=multi-touch), byte[1]=contact_count
    // Then per-contact: status(1), id(1), x_lo(1), x_hi(1), y_lo(1), y_hi(1)
    if (!_i2cRead(0x0000, _readbuf, sizeof(_readbuf))) return 0;

    uint8_t contacts = _readbuf[1] & 0x0F;
    if (contacts == 0 || contacts > max_touch_points) return 0;

    uint8_t result = 0;
    for (int i = 0; i < contacts && result < (int)count; i++) {
        uint8_t *p = &_readbuf[2 + i * 5];
        uint8_t st = p[0];
        if (!(st & 0x01)) continue;  // finger not active

        tp[result].id   = p[1] & 0x0F;
        tp[result].x    = p[2] | ((p[3] & 0x0F) << 8);
        tp[result].y    = p[4] | ((p[5] & 0x0F) << 8);
        tp[result].size = 10;
        result++;
    }
    return result;
}

} }  // namespace lgfx::v1
