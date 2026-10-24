#pragma once
#include "Bus_EPD.h"
#if defined(CONFIG_IDF_TARGET_ESP32S31)

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
  bool measureVCOM(uint32_t* vcom_mV, uint32_t timeout_ms = 500) override;
private:
  bool readReg(uint8_t address, uint8_t reg, uint8_t& value);
  bool writeReg(uint8_t address, uint8_t reg, uint8_t value);
  bool updateIo(uint8_t port, uint8_t mask, uint8_t value);
  bool _init_attempted = false;
  uint32_t _last_off = 0;
  bool _off_valid = false;
  bool _initialized = false;
  bool _scan_failed = false;
};
} }
#endif
