#pragma once

#include "Bus_EPD.h"
#include <driver/i2c_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace lgfx { inline namespace v1 {

struct Bus_EPD_V7 : public Bus_EPD
{
  void setPCA9535(i2c_port_t port, uint8_t addr) {
    _pca_port = port; _pca_addr = addr;
  }
  void setTPS65185(i2c_port_t port, uint8_t addr) {
    _tps_port = port; _tps_addr = addr;
  }
  void setI2CPins(int sda, int scl) {
    _i2c_sda = sda; _i2c_scl = scl;
  }
  void setVCOM(unsigned vcom_mV) { _vcom_mV = vcom_mV; }
  void setMC2Upseq(bool en) { _mc2_upseq = en; }

  bool init(void) override;
  bool powerControl(bool flg_on) override;
  bool measureVCOM(uint32_t* vcom_mV, uint32_t timeout_ms = 500) override;
  bool supports2DContinuousScan(void) const override;
  bool write2DFrame(const uint8_t* data, size_t length,
                    uint32_t timeout_ms, uint32_t* scan_time_us) override;
  bool write2DStream(epd_2d_line_renderer_t renderer, void* context,
                     uint32_t timeout_ms,
                     epd_2d_stream_stats_t* stats) override;

  // ─── PCA9535 Key Input (Port 0: IO0_0 ~ IO0_7) ─────────────────
  // Call once after init() to enable key scanning via the INT pin.
  // int_pin: ESP32 GPIO connected to PCA9535 INT (38 on MyPaperS3)
  void setupKeyInput(int int_pin);

  // Read PCA9535 Port 0 directly. Returns bitmask: 0=pressed, 1=released.
  // Bit 0 = IO0_0 … Bit 7 = IO0_7.
  uint8_t scanKeys(void);

  // Wait for a key event (INT falling edge) and return the first
  // pressed key index 0–7, or -1 on timeout.  Pass portMAX_DELAY
  // to block indefinitely.
  int getPressedKey(TickType_t timeout_ms = portMAX_DELAY);

private:
  i2c_port_t _pca_port = I2C_NUM_1;
  uint8_t    _pca_addr = 0x20;
  i2c_port_t _tps_port = I2C_NUM_1;
  uint8_t    _tps_addr = 0x68;
  int        _i2c_sda  = 7;
  int        _i2c_scl  = 8;
  unsigned   _vcom_mV  = 2560;
  bool       _mc2_upseq = false;

  // Key input state
  int _key_int_pin = -1;
  SemaphoreHandle_t _key_sem = nullptr;  // binary semaphore, given from ISR

  static void IRAM_ATTR _key_isr(void *arg);

  // Delegated to static helpers in .cpp
  void _pca_write(uint8_t reg, uint8_t val);
  uint8_t _pca_read(uint8_t reg);
  void _pca_pin_hi(uint8_t bit);
  void _pca_pin_lo(uint8_t bit);
  void _tps_write(uint8_t reg, uint8_t val);
  uint8_t _tps_read(uint8_t reg);
  void _tps_set_vcom(unsigned vcom_mV);
  void _tps_set_upseq_carta1300(void);
  bool _wait_tps_pwrgood(uint32_t timeout_ms);
  void _restore_after_vcom_measure(bool was_powered_on);
};

} }
