#include "Bus_EPD_S31.hpp"
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#include "../common.hpp"
#include <algorithm>
#include <cstring>
#include <esp_cache.h>
#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

namespace lgfx { inline namespace v1 {
static constexpr char TAG[] = "EPD_S31";
static constexpr size_t FRAME_BYTES = 1216 * 684 / 4;
static constexpr uint8_t IO_ADDR = 0x20, TPS_ADDR = 0x68;

Bus_EPD_S31::~Bus_EPD_S31() {
  if (_pwr_on) powerControl(false);
  if (_ckv) { rmt_disable(_ckv); rmt_del_channel(_ckv); }
  if (_encoder) rmt_del_encoder(_encoder);
  if (_rgb) esp_lcd_panel_del(_rgb);
  if (_frame_done) vSemaphoreDelete(_frame_done);
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
bool IRAM_ATTR Bus_EPD_S31::frameDone(esp_lcd_panel_handle_t,
    const esp_lcd_rgb_panel_event_data_t*, void* ctx) {
  BaseType_t wake = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<Bus_EPD_S31*>(ctx)->_frame_done, &wake);
  return wake == pdTRUE;
}
bool Bus_EPD_S31::init() {
  if (_initialized) return true;
  if (_rgb || _frame_done) return false; // fail closed after partial initialization
  if (!i2c::init(1, 0, 1).has_value()) return false;
  // Preserve unrelated outputs: P05 supplies the screen, P12 resets touch.
  uint8_t cfg0, cfg1;
  if (!readReg(IO_ADDR, 6, cfg0) || !readReg(IO_ADDR, 7, cfg1)
      || !updateIo(0, 0x27, 0x20) || !updateIo(1, 0x04, 0)
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
  _frame_done = xSemaphoreCreateBinary();
  if (!_frame_done) return false;
  esp_lcd_rgb_panel_config_t rgb = {};
  rgb.clk_src = LCD_CLK_SRC_DEFAULT;
  rgb.data_width = 8;
  // No color conversion: each byte is four packed 2-bit EPD drive codes.
  rgb.in_color_format = LCD_COLOR_FMT_GRAY8;
  rgb.out_color_format = LCD_COLOR_FMT_GRAY8;
  rgb.num_fbs = 1;
  rgb.dma_burst_size = 64;
  rgb.flags.fb_in_psram = true;
  rgb.flags.refresh_on_demand = true;
  rgb.timings.pclk_hz = MYPAPER_EPD_2D_PIXEL_CLOCK_HZ;
  rgb.timings.h_res = 304;
  rgb.timings.v_res = 684;
  rgb.timings.hsync_pulse_width = MYPAPER_EPD_2D_LE_HIGH_PCLKS;
  rgb.timings.hsync_back_porch = MYPAPER_EPD_2D_LINE_FRONT_PCLKS;
  rgb.timings.hsync_front_porch = 340 - 304 - rgb.timings.hsync_pulse_width - rgb.timings.hsync_back_porch;
  rgb.timings.vsync_pulse_width = 1;
  rgb.timings.vsync_back_porch = 0;
  rgb.timings.vsync_front_porch = 1;
  rgb.timings.flags.hsync_idle_low = true;
  rgb.timings.flags.de_idle_high = true;
  rgb.hsync_gpio_num = static_cast<gpio_num_t>(_config.pin_le);
  rgb.vsync_gpio_num = GPIO_NUM_NC;
  rgb.de_gpio_num = static_cast<gpio_num_t>(_config.pin_sph);
  rgb.pclk_gpio_num = static_cast<gpio_num_t>(_config.pin_cl);
  rgb.disp_gpio_num = GPIO_NUM_NC;
  for (size_t i = 0; i < 8; ++i) rgb.data_gpio_nums[i] = static_cast<gpio_num_t>(_config.pin_data[i]);
  esp_err_t err = esp_lcd_new_rgb_panel(&rgb, &_rgb);
  if (err != ESP_OK) { ESP_LOGE(TAG, "RGB init: %s", esp_err_to_name(err)); _rgb = nullptr; return false; }
  esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
  callbacks.on_vsync = frameDone;
  if (esp_lcd_rgb_panel_register_event_callbacks(_rgb, &callbacks, this) != ESP_OK
      || esp_lcd_panel_reset(_rgb) != ESP_OK || esp_lcd_panel_init(_rgb) != ESP_OK
      || esp_lcd_rgb_panel_get_frame_buffer(_rgb, 1, reinterpret_cast<void**>(&_rgb_frame)) != ESP_OK) return false;
  rmt_tx_channel_config_t rmt = {};
  rmt.gpio_num = static_cast<gpio_num_t>(_config.pin_ckv);
  rmt.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt.resolution_hz = 10000000;
  rmt.mem_block_symbols = 64;
  rmt.trans_queue_depth = 1;
  rmt_copy_encoder_config_t encoder = {};
  if (rmt_new_tx_channel(&rmt, &_ckv) != ESP_OK
      || rmt_new_copy_encoder(&encoder, &_encoder) != ESP_OK
      || rmt_enable(_ckv) != ESP_OK) return false;
  ESP_LOGI(TAG, "8-bit RGB ready: 20MHz 17us/row, CKV=16.3us VCOM=%u mV", MYPAPER_EPD_VCOM_MV);
  _initialized = true;
  return true;
}
bool Bus_EPD_S31::powerControl(bool on) {
  if (!on) {
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 0);
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_pwr), 0);
    const bool ok = updateIo(0, 0x07, 0);
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
bool Bus_EPD_S31::write2DFrame(const uint8_t* data, size_t length,
    uint32_t timeout_ms, uint32_t* elapsed) {
  if (elapsed) *elapsed = 0;
  if (_scan_failed || !_pwr_on || !_rgb_frame || !data || length != FRAME_BYTES) return false;
  // The driver's framebuffer is exclusively owned until frame + CKV finish.
  memcpy(_rgb_frame, data, length);
  if (esp_cache_msync(_rgb_frame, length, ESP_CACHE_MSYNC_FLAG_DIR_C2M
      | ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK) return false;
  while (xSemaphoreTake(_frame_done, 0) == pdTRUE) {}
  rmt_symbol_word_t symbol = {};
  symbol.level0 = 1; symbol.duration0 = MYPAPER_EPD_2D_CKV_HIGH_DUS;
  symbol.level1 = 0; symbol.duration1 = MYPAPER_EPD_2D_LINE_PERIOD_US * 10 - symbol.duration0;
  rmt_transmit_config_t tx = {};
  tx.loop_count = 689;
  const int64_t start = esp_timer_get_time();
  gpio_set_level(static_cast<gpio_num_t>(_config.pin_spv), 0);
  if (rmt_transmit(_ckv, _encoder, &symbol, sizeof(symbol), &tx) != ESP_OK) {
    _scan_failed = true;
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 0);
    return false;
  }
  esp_rom_delay_us(MYPAPER_EPD_2D_LINE_PERIOD_US);
  gpio_set_level(static_cast<gpio_num_t>(_config.pin_spv), 1);
  esp_rom_delay_us(MYPAPER_EPD_2D_LINE_PERIOD_US + MYPAPER_EPD_2D_CKV_HIGH_DUS / 10);
  const esp_err_t submit = esp_lcd_rgb_panel_refresh(_rgb);
  const bool done = submit == ESP_OK && xSemaphoreTake(_frame_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
  const bool ckv_done = rmt_tx_wait_all_done(_ckv, timeout_ms) == ESP_OK;
  if (elapsed) *elapsed = esp_timer_get_time() - start;
  if (!done || !ckv_done) {
    // Stop output immediately; never overwrite an in-flight buffer on retry.
    gpio_set_level(static_cast<gpio_num_t>(_config.pin_oe), 0);
    _scan_failed = true;
    ESP_LOGE(TAG, "scan failed: submit=%s frame=%d CKV=%d", esp_err_to_name(submit), done, ckv_done);
    return false;
  }
  return true;
}
} }
#endif
