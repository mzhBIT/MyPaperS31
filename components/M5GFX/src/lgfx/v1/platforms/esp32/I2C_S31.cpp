#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#include "../common.hpp"
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace lgfx { inline namespace v1 { namespace i2c {
struct Port {
  i2c_master_bus_handle_t bus = nullptr;
  SemaphoreHandle_t lock = nullptr;
  int sda = -1, scl = -1;
};
static Port ports[SOC_I2C_NUM];
static cpp::result<void, error_t> checked(esp_err_t e) {
  if (e == ESP_OK) return {};
  return cpp::fail(error_t::connection_lost);
}
cpp::result<void, error_t> init(int n, int sda, int scl) {
  if (n < 0 || n >= SOC_I2C_NUM) return checked(ESP_ERR_INVALID_ARG);
  auto& p = ports[n];
  if (p.bus) return checked(p.sda == sda && p.scl == scl ? ESP_OK : ESP_ERR_INVALID_STATE);
  i2c_master_bus_config_t c = {};
  c.i2c_port = n; c.sda_io_num = static_cast<gpio_num_t>(sda);
  c.scl_io_num = static_cast<gpio_num_t>(scl);
  c.clk_source = I2C_CLK_SRC_DEFAULT; c.glitch_ignore_cnt = 7;
  c.flags.enable_internal_pullup = true;
  p.lock = xSemaphoreCreateMutex();
  if (!p.lock) return checked(ESP_ERR_NO_MEM);
  auto e = i2c_new_master_bus(&c, &p.bus);
  if (e != ESP_OK) { vSemaphoreDelete(p.lock); p.lock = nullptr; return checked(e); }
  p.sda = sda; p.scl = scl; return {};
}
cpp::result<void, error_t> init(int n) {
  return n >= 0 && n < SOC_I2C_NUM && ports[n].bus ? checked(ESP_OK) : checked(ESP_ERR_INVALID_STATE);
}
cpp::result<void, error_t> setPins(int n, int sda, int scl) { return init(n, sda, scl); }
cpp::result<int, error_t> getPinSDA(int n) {
  if (n < 0 || n >= SOC_I2C_NUM) return cpp::fail(error_t::connection_lost);
  return ports[n].sda;
}
cpp::result<int, error_t> getPinSCL(int n) {
  if (n < 0 || n >= SOC_I2C_NUM) return cpp::fail(error_t::connection_lost);
  return ports[n].scl;
}
cpp::result<void, error_t> release(int n) {
  if (n < 0 || n >= SOC_I2C_NUM) return checked(ESP_ERR_INVALID_ARG);
  auto& p = ports[n];
  if (!p.bus) return {};
  xSemaphoreTake(p.lock, portMAX_DELAY);
  auto e = i2c_del_master_bus(p.bus);
  if (e == ESP_OK) p.bus = nullptr;
  xSemaphoreGive(p.lock);
  return checked(e);
}
cpp::result<void, error_t> transactionWriteRead(int n, int addr,
    const uint8_t* w, uint8_t wn, uint8_t* r, size_t rn, uint32_t freq) {
  if (n < 0 || n >= SOC_I2C_NUM || !ports[n].bus) return checked(ESP_ERR_INVALID_STATE);
  auto& p = ports[n];
  if (xSemaphoreTake(p.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    if (addr == 0x20 || addr == 0x68)
      ESP_LOGW("I2C_S31", "port=%d addr=%02x bus lock timeout (100 ms)", n, addr);
    return checked(ESP_ERR_TIMEOUT);
  }
  i2c_device_config_t c = {};
  c.dev_addr_length = I2C_ADDR_BIT_LEN_7; c.device_address = addr; c.scl_speed_hz = freq;
  i2c_master_dev_handle_t dev = nullptr;
  auto e = i2c_master_bus_add_device(p.bus, &c, &dev);
  const char* stage = "add-device";
  if (e == ESP_OK) {
    stage = "transfer";
    if (wn && rn) e = i2c_master_transmit_receive(dev, w, wn, r, rn, 100);
    else if (wn) e = i2c_master_transmit(dev, w, wn, 100);
    else if (rn) e = i2c_master_receive(dev, r, rn, 100);
    else e = ESP_ERR_INVALID_ARG;
    i2c_master_bus_rm_device(dev);
  }
  if (e != ESP_OK && (addr == 0x20 || addr == 0x68)) {
    // Include TPS failures so a missing supply/ACK can be distinguished from
    // bus contention. Rate limit each device separately to keep key logs quiet.
    static TickType_t last_error[2] = {};
    static bool reported[2] = {};
    const unsigned device = addr == 0x68 ? 1 : 0;
    const TickType_t now = xTaskGetTickCount();
    if (!reported[device] || TickType_t(now-last_error[device]) >= pdMS_TO_TICKS(1000)) {
      ESP_LOGW("I2C_S31", "port=%d addr=%02x reg=%02x stage=%s write=%u read=%u error=%s (0x%x)",
               n, addr, wn && w ? w[0] : 0xff, stage, unsigned(wn), unsigned(rn),
               esp_err_to_name(e), unsigned(e));
      last_error[device] = now; reported[device] = true;
    }
  }
  xSemaphoreGive(p.lock); return checked(e);
}
cpp::result<void, error_t> transactionWrite(int n, int addr, const uint8_t* w, uint8_t wn, uint32_t f) {
  return transactionWriteRead(n, addr, w, wn, nullptr, 0, f);
}
cpp::result<void, error_t> transactionRead(int n, int addr, uint8_t* r, uint8_t rn, uint32_t f) {
  return transactionWriteRead(n, addr, nullptr, 0, r, rn, f);
}
cpp::result<uint8_t, error_t> readRegister8(int n, int addr, uint8_t reg, uint32_t f) {
  uint8_t v;
  auto e = transactionWriteRead(n, addr, &reg, 1, &v, 1, f);
  if (e.has_error()) return cpp::fail(e.error());
  return v;
}
cpp::result<void, error_t> writeRegister8(int n, int addr, uint8_t reg, uint8_t v, uint8_t mask, uint32_t f) {
  if (mask) { auto old = readRegister8(n, addr, reg, f); if (old.has_error()) return cpp::fail(old.error()); v |= old.value() & mask; }
  uint8_t w[] = {reg, v}; return transactionWrite(n, addr, w, 2, f);
}
// Pin switching during autodetection is not used on the fixed S31 Board.
i2c_temporary_switcher_t::i2c_temporary_switcher_t(int n, int sda, int scl) : _i2c_port(n) { init(n, sda, scl); }
void i2c_temporary_switcher_t::restore() {}
} } }
#endif
