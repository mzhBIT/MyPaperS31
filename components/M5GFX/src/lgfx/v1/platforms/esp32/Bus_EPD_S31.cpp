#include "Bus_EPD_S31.hpp"
#include "EpdScan2DContinuous.hpp"
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#include "../common.hpp"
#include <algorithm>
#include <cstring>
#include <esp_cache.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>

namespace lgfx { inline namespace v1 {
static constexpr char TAG[] = "EPD_S31";
static constexpr size_t FRAME_BYTES = 1216 * 684 / 4;
static constexpr uint64_t ROW_CLOCK_PRODUCT =
    uint64_t(MYPAPER_EPD_2D_PIXEL_CLOCK_HZ) * MYPAPER_EPD_2D_LINE_PERIOD_US;
static constexpr uint32_t ROW_CLOCKS = ROW_CLOCK_PRODUCT / 1000000u;
static_assert(ROW_CLOCK_PRODUCT % 1000000u == 0,
              "RGB row must contain an integral number of pixel clocks");
static_assert(ROW_CLOCKS > 304u + MYPAPER_EPD_2D_LE_HIGH_PCLKS
                                     + MYPAPER_EPD_2D_LINE_FRONT_PCLKS,
              "RGB row must leave room for data and porches");
static_assert(MYPAPER_EPD_2D_CKV_HIGH_DUS > 0
              && MYPAPER_EPD_2D_CKV_HIGH_DUS < MYPAPER_EPD_2D_LINE_PERIOD_US * 10u,
              "CKV high time must be shorter than the RMT row period");
static constexpr uint8_t IO_ADDR = 0x20, TPS_ADDR = 0x68;

Bus_EPD_S31::~Bus_EPD_S31() {
  if (_pwr_on) powerControl(false);
}

bool Bus_EPD_S31::readReg(uint8_t address, uint8_t reg, uint8_t& value) {
  auto r = i2c::readRegister8(1, address, reg, 100000);
  if (!r.has_value()) { ESP_LOGE(TAG, "I2C read %02x/%02x failed", address, reg); return false; }
  value = r.value(); return true;
}
bool Bus_EPD_S31::writeReg(uint8_t address, uint8_t reg, uint8_t value) {
  const bool ok = i2c::writeRegister8(1, address, reg, value, 0, 100000).has_value();
  if (!ok) ESP_LOGE(TAG, "I2C write %02x/%02x failed", address, reg);
  return ok;
}
bool Bus_EPD_S31::updateIo(uint8_t port, uint8_t mask, uint8_t value) {
  uint8_t out;
  return readReg(IO_ADDR, 2 + port, out)
      && writeReg(IO_ADDR, 2 + port, (out & ~mask) | (value & mask));
}
bool Bus_EPD_S31::init() {
  if (_initialized) return true;
  if (_init_attempted) return false; // fail closed after partial initialization
  _init_attempted = true;
  if (!i2c::init(1, 0, 1).has_value()) return false;
  // Preserve unrelated outputs: P05 supplies the screen, P12 resets touch.
  uint8_t cfg0, cfg1;
  bool config_ready = false;
  // Read-only startup handshake: allow a brief rail/bus settling interval.
  // Never continue with guessed direction bits or repeat a partial rail setup.
  for (unsigned attempt = 1; attempt <= 3; ++attempt) {
    if (readReg(IO_ADDR, 6, cfg0) && readReg(IO_ADDR, 7, cfg1)) {
      config_ready = true;
      if (attempt > 1) ESP_LOGW(TAG, "TCA config read recovered: attempt=%u", attempt);
      break;
    }
    if (attempt < 3) delay(10);
  }
  if (!config_ready) {
    ESP_LOGE(TAG, "TCA startup handshake failed after 3 attempts; panel init aborted");
    return false;
  }
  if (!updateIo(0, 0x27, 0x20) || !updateIo(1, 0x04, 0)
      || !writeReg(IO_ADDR, 6, (cfg0 & ~0x27) | 0x80)
      || !writeReg(IO_ADDR, 7, cfg1 & ~0x04)) return false;
  delay(10);
  if (!updateIo(1, 0x04, 0x04)) return false;
  delay(50);
  const int pins[] = {_config.pin_oe, _config.pin_pwr, 37, _config.pin_spv};
  for (int pin : pins) {
    gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(pin), pin == _config.pin_spv);
  }
  if (!epd2d_continuous_scan_init(_config, 1216, 684)) return false;
  ESP_LOGI(TAG, "shared P4 HAL/LL scan ready: bus=8 pclk=%lu Hz row_clocks=%lu period=%u us CKV=%u.%u us VCOM=%u mV",
           (unsigned long)MYPAPER_EPD_2D_PIXEL_CLOCK_HZ, (unsigned long)ROW_CLOCKS,
           unsigned(MYPAPER_EPD_2D_LINE_PERIOD_US), unsigned(MYPAPER_EPD_2D_CKV_HIGH_DUS / 10),
           unsigned(MYPAPER_EPD_2D_CKV_HIGH_DUS % 10), MYPAPER_EPD_VCOM_MV);
  _initialized = true;
  return true;
}
bool Bus_EPD_S31::powerControl(bool on) {
  if (!on) {
    // Mirror the P4 quiet shutdown instead of clearing every control bit at
    // once. Disable the panel output and park the scan lines while the bias
    // rails are still up, hold that state for 20 ms so the source/gate drivers
    // can discharge, then drop PWRUP before WAKEUP. The previous one-shot
    // shutdown cut the rails with the last frame still on the source lines,
    // which left residual charge that accumulated as ghosting over successive
    // epd_text (DU4) refreshes.  pin_pwr is this board's MODE line (GPIO19),
    // not a PMIC PWRUP: P4 asserts MODE=H for sequential scanning and drops it
    // during shutdown.
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_pwr), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_le), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_ckv), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_cl), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_sph), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_spv), 0);
    for (int i = 0; i < _config.bus_width; ++i) {
      gpio_set_level(static_cast<gpio_num_t>(_config.pin_data[i]), 0);
    }
    // TCA9555 port 0: VCOM_CTRL=P00, PWRUP=P01, WAKEUP=P02. Keep all three
    // asserted while the panel settles, then drop PWRUP, then WAKEUP.
    bool ok = updateIo(0, 0x07, 0x07);
    delay(20);
    ok = updateIo(0, 0x07, 0x04) && ok;
    delay(1);
    ok = updateIo(0, 0x07, 0x00) && ok;
    _pwr_on = false; _last_off = millis(); _off_valid = true;
    return ok;
  }
  if (!_initialized || _scan_failed) return false;
  if (_pwr_on) return true;
  if (_off_valid && millis() - _last_off < 250) delay(250 - (millis() - _last_off));
  uint8_t pg = 0;
  bool ok = updateIo(0, 0x07, 0x04);
  delay(50);
  const unsigned vcom = MYPAPER_EPD_VCOM_MV / 10;
  ok = ok && writeReg(TPS_ADDR, 0x09, 0xe4) && writeReg(TPS_ADDR, 0x0a, 0x55)
      && writeReg(TPS_ADDR, 0x03, vcom & 0xff) && writeReg(TPS_ADDR, 0x04, (vcom >> 8) & 1)
      && writeReg(TPS_ADDR, 0x01, 0x3f) && updateIo(0, 0x07, 0x07);
  const uint32_t start = millis();
  while (ok && millis() - start < 250) {
    if (!readReg(TPS_ADDR, 0x0f, pg)) { ok = false; break; }
    if (pg == 0xfa) break;
    delay(2);
  }
  if (!ok || pg != 0xfa) {
    ESP_LOGE(TAG, "TPS power-on failed PG=%02x", pg); powerControl(false); return false;
  }
  gpio_set_level(static_cast<gpio_num_t>(_config.pin_pwr), 1);
  gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 1);
  _pwr_on = true;
  return true;
}
bool Bus_EPD_S31::measureVCOM(uint32_t* vcom_mV, uint32_t timeout_ms) {
  if (!vcom_mV) return false;
  *vcom_mV = 0;
  // Panel_EPD_2D::measureVCOM has already drained the refresh queue. Do not
  // attempt to measure while a failed scan may still own its DMA source.
  if (!_initialized || _scan_failed || !timeout_ms) return false;
  constexpr uint8_t VCOM1 = 0x03, VCOM2 = 0x04, ENABLE = 0x01;
  constexpr uint8_t INT1 = 0x07, PG = 0x0f;
  constexpr uint8_t ACQ = 0x80, PROG = 0x40, HIZ = 0x20, ACQC = 0x02;
  const bool was_on = _pwr_on;
  const int old_oe = gpio_get_level(static_cast<gpio_num_t>(_config.pin_oe));
  const int old_mode = gpio_get_level(static_cast<gpio_num_t>(_config.pin_pwr));
  const int old_spv = gpio_get_level(static_cast<gpio_num_t>(_config.pin_spv));
  // refresh_on_demand is idle here and Panel_EPD_2D has drained all scans.
  // XSTL/SPH is RGB DE during refresh. Save the complete matrix route,
  // including inversion/OE selection, before temporarily driving it as GPIO.
  const uint32_t old_sph_route = GPIO.func_out_sel_cfg[_config.pin_sph].val;
  bool sph_taken = false;
  uint8_t old_v1 = 0, old_v2 = 0, old_enable = 0;
  uint8_t int1 = 0, pg = 0, result_lsb = 0, result_msb = 0;
  bool saved = false, measured = false;
  const char* stage = "wake";
  gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 0);
  do {
    if (!was_on) {
      if (_off_valid && millis() - _last_off < 250)
        delay(250 - (millis() - _last_off));
      // TCA9555 port 0: VCOM_CTRL=P00, PWRUP=P01, WAKEUP=P02.
      if (!updateIo(0, 0x07, 0x04)) break;
      delay(50);
    }
    stage = "save-registers";
    if (!readReg(TPS_ADDR, VCOM1, old_v1)
        || !readReg(TPS_ADDR, VCOM2, old_v2)
        || !readReg(TPS_ADDR, ENABLE, old_enable)) break;
    saved = true;
    stage = "panel-control";
    // Match the verified P4 sequence: MODE/VCOM_CTRL low, OE/WAKEUP
    // high, SPV low while raising PWRUP. Do NOT scan a neutral RGB frame.
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_spv), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_pwr), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 1);
    if (!updateIo(0, 0x07, 0x04)
        || !updateIo(0, 0x07, 0x06)) break;
    delay(1);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_spv), 1);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_sph), 1);
    esp_rom_gpio_connect_out_signal(_config.pin_sph, SIG_GPIO_OUT_IDX, false, false);
    sph_taken = true;
    delay(20);
    if (!writeReg(TPS_ADDR, ENABLE, 0x3f)) break;
    stage = "power-good";
    const uint32_t power_start = millis();
    bool rails_ok = false;
    do {
      if (!readReg(TPS_ADDR, PG, pg)) break;
      if ((pg & 0xfa) == 0xfa) { rails_ok = true; break; }
      delay(2);
    } while (millis() - power_start < timeout_ms);
    if (!rails_ok) break;

    stage = "acquire";
    if (!readReg(TPS_ADDR, INT1, int1)
        || !writeReg(TPS_ADDR, VCOM2, HIZ | ACQ)) break; // P4: 0xA0, never PROG
    const uint32_t acquire_start = millis();
    do {
      if (!readReg(TPS_ADDR, INT1, int1)) break;
      if (int1 & ACQC) {
        measured = readReg(TPS_ADDR, VCOM1, result_lsb)
                && readReg(TPS_ADDR, VCOM2, result_msb);
        break;
      }
      delay(1);
    } while (millis() - acquire_start < timeout_ms);
  } while (false);

  // One cleanup path for success, timeout, I2C failure and scan failure.
  // Lower PWRUP before reconnecting VCOM. Keep WAKEUP high for I2C restore.
  gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 0);
  bool restored = updateIo(0, 0x07, 0x04);
  delay(50);
  // Restore peripheral ownership with panel output disabled on every exit,
  // before powerControl can re-enable OE. No LCD reset or frame allocation.
  if (sph_taken) GPIO.func_out_sel_cfg[_config.pin_sph].val = old_sph_route;
  if (saved) {
    const bool v1_ok = writeReg(TPS_ADDR, VCOM1, old_v1);
    const bool v2_ok = writeReg(TPS_ADDR, VCOM2, old_v2 & ~(ACQ | PROG));
    const bool en_ok = writeReg(TPS_ADDR, ENABLE, old_enable & 0x3f);
    restored = v1_ok && v2_ok && en_ok && restored;
  }
  _pwr_on = false;
  _last_off = millis();
  _off_valid = true;
  if (was_on && restored && !_scan_failed) {
    restored = powerControl(true);
    if (restored && saved) {
      // powerControl programs the normal configured voltage. Preserve the
      // actual pre-measurement register state as well (not the ADC result).
      const bool v1_ok = writeReg(TPS_ADDR, VCOM1, old_v1);
      const bool v2_ok = writeReg(TPS_ADDR, VCOM2, old_v2 & ~(ACQ | PROG));
      const bool en_ok = writeReg(TPS_ADDR, ENABLE, old_enable & 0x3f);
      restored = v1_ok && v2_ok && en_ok;
    }
    if (restored) {
      gpio_set_level(static_cast<gpio_num_t>(_config.pin_pwr), old_mode);
      gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), old_oe);
    } else {
      powerControl(false);
    }
  } else {
    const bool off_ok = updateIo(0, 0x07, 0);
    restored = off_ok && restored;
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_pwr), 0);
    if (was_on) restored = false; // Remain safely off after restoration failure.
  }
  gpio_set_level(static_cast<gpio_num_t>(_config.pin_spv), old_spv);
  const uint32_t result = (result_lsb | (uint32_t(result_msb & 1) << 8)) * 10;
  ESP_LOGI(TAG, "VCOM measurement P4-sequence: ok=%d stage=%s INT1=0x%02x PG=0x%02x kickback=%lu mV restored=%d was_on=%d",
           measured && restored, stage, int1, pg, (unsigned long)result, restored, was_on);
  if (!measured || !restored) return false;
  *vcom_mV = result;
  return true;
}
bool Bus_EPD_S31::write2DFrame(const uint8_t* data, size_t length,
    uint32_t timeout_ms, uint32_t* elapsed) {
  if (elapsed) *elapsed = 0;
  if (!_initialized || _scan_failed || !_pwr_on || !data || length != FRAME_BYTES) return false;
  if (!epd2d_continuous_scan_frame(data, length, timeout_ms, elapsed)) {
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 0);
    _scan_failed = true;
    return false;
  }
  return true;
}
} }
#endif
