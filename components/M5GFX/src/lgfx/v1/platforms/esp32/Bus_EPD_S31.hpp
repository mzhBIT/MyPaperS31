#pragma once
#include "Bus_EPD.h"
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#include <esp_lcd_panel_rgb.h>
#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace lgfx { inline namespace v1 {
// Dedicated S31/TCA9555 backend. No P4 registers or pin assignments here.
class Bus_EPD_S31 : public Bus_EPD {
public:
  ~Bus_EPD_S31() override;
  bool init() override;
  // RGB/RMT own the scan pins; drawing transactions must not pulse them.
  void beginTransaction() override {}
  void endTransaction() override {}
  bool powerControl(bool on) override;
  bool supports2DContinuousScan() const override { return _initialized; }
  bool write2DFrame(const uint8_t*, size_t, uint32_t, uint32_t*) override;
  bool measureVCOM(uint32_t*, uint32_t = 500) override { return false; }
private:
  bool readReg(uint8_t address, uint8_t reg, uint8_t& value);
  bool writeReg(uint8_t address, uint8_t reg, uint8_t value);
  bool updateIo(uint8_t port, uint8_t mask, uint8_t value);
  static bool IRAM_ATTR frameDone(esp_lcd_panel_handle_t,
      const esp_lcd_rgb_panel_event_data_t*, void*);
  esp_lcd_panel_handle_t _rgb = nullptr;
  rmt_channel_handle_t _ckv = nullptr;
  rmt_encoder_handle_t _encoder = nullptr;
  SemaphoreHandle_t _frame_done = nullptr;
  uint8_t* _rgb_frame = nullptr;
  uint32_t _last_off = 0;
  bool _off_valid = false;
  bool _initialized = false;
  bool _scan_failed = false;
};
} }
#endif
