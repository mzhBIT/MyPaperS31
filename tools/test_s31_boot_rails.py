"""Generate a native test of actual S31 display init after retained TCA states."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else '.codex-build-cache/s31-boot-rails')
out.mkdir(parents=True, exist_ok=True)
source = (root / 'components/M5GFX/src/lgfx/v1/platforms/esp32/Bus_EPD_S31.cpp').read_text(encoding='utf-8')
methods = source[source.index('bool Bus_EPD_S31::updateIo('):source.index('bool Bus_EPD_S31::powerControl(')]
(out / 'test.cpp').write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr uint8_t IO_ADDR = 0x20;
using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1;
unsigned elapsed, scans, gpios;
void delay(unsigned ms) { elapsed += ms; }
void gpio_set_direction(int, int) { ++gpios; }
void gpio_set_level(int, int) { ++gpios; }
namespace i2c {
struct Result { bool has_value() const { return true; } };
Result init(int, int, int) { return {}; }
}
struct Config { int pin_oe = 18, pin_pwr = 19, pin_spv = 35; };
bool epd2d_continuous_scan_init(Config&, int w, int h) {
    assert(w == 1216 && h == 684); ++scans; return true;
}
class Bus_EPD_S31 {
public:
    bool init();
    bool updateIo(uint8_t, uint8_t, uint8_t);
    bool readReg(uint8_t addr, uint8_t reg, uint8_t& value) {
        assert(addr == IO_ADDR);
        if (fail_read) return false;
        value = regs[reg]; return true;
    }
    bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
        assert(addr == IO_ADDR);
        if (fail_write) return false;
        if (stuck_p14 && reg == 3) value &= ~0x10;
        regs[reg] = value; return true;
    }
    uint8_t regs[8]{};
    bool fail_read = false, fail_write = false, stuck_p14 = false;
    bool _initialized = false, _init_attempted = false;
    Config _config;
};
''' + methods + r'''
int main() {
    // Exhaust all inherited port-1 output/config values, including emulator's
    // P14-low/output state and cold-boot P14-high/input state.
    for (unsigned value = 0; value < 256; ++value)
        for (unsigned cfg = 0; cfg < 256; ++cfg) {
            Bus_EPD_S31 bus;
            bus.regs[2] = 0xdb; bus.regs[3] = value;
            bus.regs[6] = 0xff; bus.regs[7] = cfg;
            elapsed = scans = gpios = 0;
            assert(bus.init() && bus._initialized && scans == 1 && gpios != 0);
            assert(bus.regs[2] == uint8_t((0xdb & ~0x27) | 0x20));
            assert(bus.regs[3] == uint8_t(value | 0x14));
            assert(bus.regs[6] == uint8_t((0xff & ~0x27) | 0x80));
            assert(bus.regs[7] == uint8_t(cfg & ~0x14));
            assert(elapsed >= 80);
            // Already initialized: no second reset or scan reconfiguration.
            assert(bus.init() && scans == 1);
        }
    for (int failure = 0; failure < 3; ++failure) {
        Bus_EPD_S31 bus;
        bus.fail_read = failure == 0;
        bus.fail_write = failure == 1;
        bus.stuck_p14 = failure == 2;
        elapsed = scans = gpios = 0;
        assert(!bus.init() && !bus._initialized && scans == 0 && gpios == 0);
        // Partial initialization cannot be repeated with guessed state.
        bus.fail_read = bus.fail_write = bus.stuck_p14 = false;
        assert(!bus.init() && scans == 0);
    }
    std::puts("S31 boot rails: 65536 retained states, unrelated bits preserved, settling and read/write/stuck-rail failures PASS");
}
''', encoding='utf-8')
print(out / 'test.cpp')
