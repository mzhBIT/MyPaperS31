#include "Bus_EPD_V7.hpp"
#include "EpdScan2DContinuous.hpp"

#if SOC_LCD_I80_SUPPORTED

#include <string.h>
#include "../common.hpp"
#include <esp_log.h>
#include <driver/gpio.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if __has_include (<esp_private/periph_ctrl.h>)
 #include <esp_private/periph_ctrl.h>
#else
 #include <driver/periph_ctrl.h>
#endif

// TPS65185 registers
#define TPS_REG_ENABLE      0x01
#define TPS_REG_VCOM1       0x03
#define TPS_REG_VCOM2       0x04
#define TPS_REG_INT_EN1     0x05
#define TPS_REG_INT1        0x07
#define TPS_REG_INT2        0x08
#define TPS_REG_UPSEQ0      0x09
#define TPS_REG_UPSEQ1      0x0A
#define TPS_REG_PG          0x0F

namespace lgfx { inline namespace v1 {

// ─── PCA9535 bit masks (port 1) ───────────────────────────────────
#define PCA_BIT_OE       0x01  // PC10
#define PCA_BIT_MODE     0x02  // PC11
#define PCA_BIT_FRONTLIGHT 0x04 // PC12 / PCA9535 IO1_2, owned by frontlight
#define PCA_BIT_PWRUP    0x08  // PC13
#define PCA_BIT_VCOMCTRL 0x10  // PC14
#define PCA_BIT_WAKEUP   0x20  // PC15
#define PCA_BIT_PWRGOOD  0x40  // PC16

static constexpr char TAG[] = "Bus_EPD_V7";

#if defined(CONFIG_IDF_TARGET_ESP32P4) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
struct epd2d_init_context_t {
    const Bus_EPD::config_t* config;
    size_t panel_width;
    size_t panel_height;
    SemaphoreHandle_t done;
    bool result;
};

static void epd2d_init_task(void* argument)
{
    auto* context = static_cast<epd2d_init_context_t*>(argument);
    ESP_LOGI(TAG, "2D scan backend init on core %d", xPortGetCoreID());
    context->result = epd2d_continuous_scan_init(*context->config,
                                                 context->panel_width,
                                                 context->panel_height);
    xSemaphoreGive(context->done);
    vTaskDelete(nullptr);
}

static bool init_epd2d_on_dedicated_core(const Bus_EPD::config_t& config,
                                         size_t panel_width,
                                         size_t panel_height)
{
    constexpr BaseType_t epd_core = portNUM_PROCESSORS > 1 ? 1 : 0;
    if (xPortGetCoreID() == epd_core) {
        return epd2d_continuous_scan_init(config, panel_width, panel_height);
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) {
        ESP_LOGE(TAG, "2D scan init semaphore allocation failed");
        return false;
    }
    epd2d_init_context_t context = {
        .config = &config,
        .panel_width = panel_width,
        .panel_height = panel_height,
        .done = done,
        .result = false,
    };
    TaskHandle_t task = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCore(
        epd2d_init_task, "epd2d_init", 6144, &context,
        configMAX_PRIORITIES - 2, &task, epd_core);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "2D scan init task creation failed");
        vSemaphoreDelete(done);
        return false;
    }
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return context.result;
}
#endif

static uint8_t make_pca_power_value(bool output_enable, bool mode, bool pwrup, bool vcom_ctrl, bool wakeup)
{
    uint8_t value = 0;
    if (output_enable) { value |= PCA_BIT_OE; }
    if (mode) { value |= PCA_BIT_MODE; }
    if (pwrup) { value |= PCA_BIT_PWRUP; }
    if (vcom_ctrl) { value |= PCA_BIT_VCOMCTRL; }
    if (wakeup) { value |= PCA_BIT_WAKEUP; }
    return value;
}

// ─── Override init ────────────────────────────────────────────────

bool Bus_EPD_V7::init(void) {
    _bus_busy = false;
    _pwr_on = false;
#if MYPAPER_EPD_USE_2D_ENGINE
    _last_power_off_ms = 0;
    _power_off_time_valid = false;
#endif
    _i80_bus_handle = nullptr;
    _io_handle = nullptr;

    // Init I2C via M5GFX register-level access.
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
    periph_module_enable(PERIPH_I2C1_MODULE);
#endif
    i2c::setPins(_pca_port, _i2c_sda, _i2c_scl);
    i2c::init(_pca_port);

    // Log PCA9535 accesses during initialization so I2C failures are visible.
    auto pca_read_init = [this](uint8_t reg) -> uint8_t {
        auto result = i2c::readRegister8(_pca_port, _pca_addr, reg, 100000);
        if (result.has_value()) {
            ESP_LOGI(TAG, "PCA9535 read OK: addr=0x%02X reg=0x%02X value=0x%02X",
                     _pca_addr, reg, result.value());
            return result.value();
        }
        ESP_LOGE(TAG, "PCA9535 read FAILED: addr=0x%02X reg=0x%02X",
                 _pca_addr, reg);
        return 0;
    };

    auto pca_write_init = [this](uint8_t reg, uint8_t value) {
        auto result = i2c::writeRegister8(_pca_port, _pca_addr, reg, value, 0x00, 100000);
        if (result.has_value()) {
            ESP_LOGI(TAG, "PCA9535 write OK: addr=0x%02X reg=0x%02X value=0x%02X",
                     _pca_addr, reg, value);
        } else {
            ESP_LOGE(TAG, "PCA9535 write FAILED: addr=0x%02X reg=0x%02X value=0x%02X",
                     _pca_addr, reg, value);
        }
    };

    // Direct GPIO control signals
    lgfx::pinMode(_config.pin_spv, lgfx::pin_mode_t::output);
    lgfx::pinMode(_config.pin_ckv, lgfx::pin_mode_t::output);
    lgfx::pinMode(_config.pin_sph, lgfx::pin_mode_t::output);
    lgfx::pinMode(_config.pin_le,  lgfx::pin_mode_t::output);
    lgfx::pinMode(_config.pin_cl,  lgfx::pin_mode_t::output);

    // Configure PCA9535 port 1: control pins as outputs, PWRGOOD as input.
    uint8_t cfg = pca_read_init(7);
    cfg &= ~(PCA_BIT_OE | PCA_BIT_MODE | PCA_BIT_PWRUP | PCA_BIT_VCOMCTRL | PCA_BIT_WAKEUP);
    cfg |= PCA_BIT_PWRGOOD;
    pca_write_init(7, cfg);

    // Start all outputs low
    uint8_t out = pca_read_init(3);
    out &= ~(PCA_BIT_OE | PCA_BIT_MODE | PCA_BIT_PWRUP | PCA_BIT_VCOMCTRL | PCA_BIT_WAKEUP);
    pca_write_init(3, out);

#if defined(CONFIG_IDF_TARGET_ESP32P4) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
    // Interrupt affinity is selected by the core which installs each driver.
    // Initialize LCD_CAM/GDMA/RMT on the same dedicated core as epd2d, keeping
    // their frame-end ISRs away from the core-0 USB Audio deadline path.
    if (!init_epd2d_on_dedicated_core(_config,
                                      MYPAPER_EPD_MEMORY_WIDTH,
                                      MYPAPER_EPD_MEMORY_HEIGHT)) {
        ESP_LOGE(TAG, "2D continuous scan initialization failed");
        return false;
    }
    ESP_LOGI(TAG, "2D continuous LCD_CAM scan selected; legacy I80 not initialized");
    return true;
#endif

    // i80 bus setup
    esp_lcd_i80_bus_config_t bus_config;
    memset(&bus_config, 0, sizeof(bus_config));
    bus_config.max_transfer_bytes = 1024;
    bus_config.clk_src = LCD_CLK_SRC_PLL160M;
    bus_config.dc_gpio_num = (gpio_num_t)_config.pin_sph;
    bus_config.wr_gpio_num = (gpio_num_t)_config.pin_cl;
    bus_config.bus_width = _config.bus_width;
    for (int i = 0; i < _config.bus_width; ++i) {
        bus_config.data_gpio_nums[i] = (gpio_num_t)_config.pin_data[i];
        lgfx::pinMode(_config.pin_data[i], lgfx::pin_mode_t::output);
    }

    esp_err_t i80_bus_err = esp_lcd_new_i80_bus(&bus_config, &_i80_bus_handle);
    if (i80_bus_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_i80_bus FAILED: %s (0x%x)",
                 esp_err_to_name(i80_bus_err), (unsigned)i80_bus_err);
        return false;
    }

    esp_lcd_panel_io_i80_config_t io_config;
    memset(&io_config, 0, sizeof(io_config));
    io_config.trans_queue_depth = 4;
    io_config.on_color_trans_done = notify_line_done;
    io_config.user_ctx = this;
    io_config.pclk_hz = _config.bus_speed;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.pclk_idle_low = 1;
    io_config.flags.pclk_active_neg = 0;
    // Match the known-good MyPaperS3 path: the I80 CS signal drives STH.
    io_config.cs_gpio_num = (gpio_num_t)_config.pin_sph;

    esp_err_t panel_io_err = esp_lcd_new_panel_io_i80(_i80_bus_handle, &io_config, &_io_handle);
    if (panel_io_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i80 FAILED: pclk=%lu Hz err=%s (0x%x)",
                 (unsigned long)io_config.pclk_hz,
                 esp_err_to_name(panel_io_err), (unsigned)panel_io_err);
        return false;
    }

    const char* sth_source = "CS";
    ESP_LOGI(TAG, "I80 ready: width=%u pclk=%lu Hz idle_low=1 active_neg=0 STH/%s=%d CKH=%d CKV=%d STV=%d LE=%d",
             (unsigned)_config.bus_width, (unsigned long)_config.bus_speed,
             sth_source, (int)_config.pin_sph, (int)_config.pin_cl, (int)_config.pin_ckv,
             (int)_config.pin_spv, (int)_config.pin_le);

    return true;
}

bool Bus_EPD_V7::supports2DContinuousScan(void) const {
#if defined(CONFIG_IDF_TARGET_ESP32P4) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
    return true;
#else
    return false;
#endif
}

bool Bus_EPD_V7::write2DFrame(const uint8_t* data, size_t length,
                              uint32_t timeout_ms, uint32_t* scan_time_us) {
#if defined(CONFIG_IDF_TARGET_ESP32P4) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
    return epd2d_continuous_scan_frame(data, length, timeout_ms, scan_time_us);
#else
    (void)data;
    (void)length;
    (void)timeout_ms;
    if (scan_time_us) { *scan_time_us = 0; }
    return false;
#endif
}

bool Bus_EPD_V7::write2DStream(epd_2d_line_renderer_t renderer, void* context,
                               uint32_t timeout_ms,
                               epd_2d_stream_stats_t* stats) {
#if defined(CONFIG_IDF_TARGET_ESP32P4) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN \
 && MYPAPER_EPD_2D_LINE_STREAM
    return epd2d_continuous_scan_stream(renderer, context, timeout_ms, stats);
#else
    (void)renderer;
    (void)context;
    (void)timeout_ms;
    if (stats) { *stats = {}; }
    return false;
#endif
}

// ─── Override powerControl ────────────────────────────────────────

bool Bus_EPD_V7::powerControl(bool flg_on) {
    // Probe PCA9535 with an address-only I2C transaction before changing power state.

    // auto pca_probe = i2c::transactionWrite(_pca_port, _pca_addr, nullptr, 0, 400000);
    // if (pca_probe.has_value()) {
    //     ESP_LOGI(TAG, "PCA9535 detected: port=%d addr=0x%02X", (int)_pca_port, _pca_addr);
    // } else {
    //     ESP_LOGE(TAG, "PCA9535 not detected: port=%d addr=0x%02X", (int)_pca_port, _pca_addr);
    // }

    // // Probe TPS65185 with an address-only I2C transaction before changing power state.
    // auto tps_probe = i2c::transactionWrite(_tps_port, _tps_addr, nullptr, 0, 400000);
    // if (tps_probe.has_value()) {
    //     ESP_LOGI(TAG, "TPS65185 detected: port=%d addr=0x%02X", (int)_tps_port, _tps_addr);
    // } else {
    //     ESP_LOGE(TAG, "TPS65185 not detected: port=%d addr=0x%02X", (int)_tps_port, _tps_addr);
    // }

    // ESP_LOGI("Bus_EPD_V7", "in powerControl(%d)", flg_on);
    
    if (_pwr_on != flg_on) {
        _pwr_on = flg_on;
        wait();
        if (flg_on) {
#if MYPAPER_EPD_USE_2D_ENGINE
            if (_power_off_time_valid) {
                const uint32_t elapsed_ms = lgfx::millis() - _last_power_off_ms;
                if (elapsed_ms < MYPAPER_EPD_2D_MIN_POWER_OFF_MS) {
                    const uint32_t settle_ms =
                        MYPAPER_EPD_2D_MIN_POWER_OFF_MS - elapsed_ms;
                    ESP_LOGI(TAG, "waiting %lu ms for TPS65185 rail discharge",
                             (unsigned long)settle_ms);
                    lgfx::delay(settle_ms);
                }
            }
#endif
            // Match epdiy v7: rewrite PCA port1 as a complete control register.
            lgfx::gpio_hi(_config.pin_spv);
            uint8_t out = make_pca_power_value(false, false, false, false, true);
            _pca_write(3, out);
            lgfx::delay(10);

            if (_mc2_upseq) {
                lgfx::delay(3);
                _tps_set_upseq_carta1300();
            }

            out = make_pca_power_value(false, false, true, true, true);
            _pca_write(3, out);
            lgfx::delay(10);

            _tps_write(TPS_REG_ENABLE, 0x3F);
            _tps_set_vcom(_vcom_mV);

            lgfx::gpio_hi(_config.pin_sph);
            if (!_wait_tps_pwrgood(200)) {
                const uint8_t int1 = _tps_read(TPS_REG_INT1);
                const uint8_t int2 = _tps_read(TPS_REG_INT2);
                ESP_LOGE(TAG, "TPS Register PG Timeout! INT1=0x%02X INT2=0x%02X",
                         int1, int2);
                // Keep the logical state asserted so the panel's subsequent
                // powerControl(false) performs a real rail/VCOM/WAKEUP cleanup.
                return false;
            }

            // E0470 gate-driver timing specifies MODE=H for normal sequential
            // G1..G684 scanning.  The previous 0x39 state left MODE low; use
            // 0x3B while panel outputs are enabled and a frame is scanned.
            out = make_pca_power_value(true, true, true, true, true);
            _pca_write(3, out);
            uint8_t pca_out = _pca_read(3);
            uint8_t pca_cfg = _pca_read(7);
            uint8_t tps_enable = _tps_read(TPS_REG_ENABLE);
            uint8_t tps_vcom1 = _tps_read(TPS_REG_VCOM1);
            uint8_t tps_vcom2 = _tps_read(TPS_REG_VCOM2);
            uint8_t tps_pg = _tps_read(TPS_REG_PG);
            // Clear any power-up/VCOM programming events before the first
            // scan. Subsequent reads then describe faults caused by a frame.
            uint8_t tps_int1 = _tps_read(TPS_REG_INT1);
            uint8_t tps_int2 = _tps_read(TPS_REG_INT2);
            ESP_LOGI(TAG,
                     "power state readback: PCA_OUT=0x%02X expected=0x%02X PCA_CFG=0x%02X "
                     "TPS_EN=0x%02X VCOM1=0x%02X VCOM2=0x%02X PG=0x%02X "
                     "INT1(clear)=0x%02X INT2(clear)=0x%02X",
                     pca_out, out, pca_cfg, tps_enable, tps_vcom1, tps_vcom2, tps_pg,
                     tps_int1, tps_int2);
            constexpr uint8_t pca_epd_mask = PCA_BIT_OE | PCA_BIT_MODE | PCA_BIT_PWRUP
                                           | PCA_BIT_VCOMCTRL | PCA_BIT_WAKEUP;
            if ((pca_out & pca_epd_mask) != (out & pca_epd_mask)) {
                ESP_LOGE(TAG,
                         "PCA EPD output readback mismatch: wrote=0x%02X read=0x%02X",
                         out & pca_epd_mask, pca_out & pca_epd_mask);
            }

        } else {
            lgfx::delay(1);

            // Capture the final frame as well; there is no following
            // powerControl(true) call after the waveform has completed.
            uint8_t pg = _tps_read(TPS_REG_PG);
            uint8_t int1 = _tps_read(TPS_REG_INT1);
            uint8_t int2 = _tps_read(TPS_REG_INT2);
            ESP_LOGI(TAG, "pre-power-off state: PG=0x%02X INT1=0x%02X INT2=0x%02X",
                     pg, int1, int2);

            // Experimental quiet shutdown: disable panel outputs first, then
            // park the source/gate/data lines before dropping the bias rails.
            uint8_t out = make_pca_power_value(false, false, true, true, true);
            _pca_write(3, out);
            lgfx::gpio_lo(_config.pin_le);
            lgfx::gpio_lo(_config.pin_ckv);
            lgfx::gpio_lo(_config.pin_cl);
            lgfx::gpio_lo(_config.pin_sph);
            lgfx::gpio_lo(_config.pin_spv);
            for (int i = 0; i < _config.bus_width; ++i) {
                lgfx::gpio_lo(_config.pin_data[i]);
            }
            lgfx::delay(20);

            out = make_pca_power_value(false, false, false, false, true);
            _pca_write(3, out);
            lgfx::delay(1);
            out = make_pca_power_value(false, false, false, false, false);
            _pca_write(3, out);
#if MYPAPER_EPD_USE_2D_ENGINE
            _last_power_off_ms = lgfx::millis();
            _power_off_time_valid = true;
#endif
        }
    }
    return true;
}

bool Bus_EPD_V7::measureVCOM(uint32_t* vcom_mV, uint32_t timeout_ms) {
    if (!vcom_mV) {
        return false;
    }
    *vcom_mV = 0;

    wait();
    bool was_powered_on = _pwr_on;

    // Match epdiy2 board_v7: ACTIVE mode, rails up, VCOM control left disabled.
    lgfx::gpio_lo(_config.pin_spv);

    uint8_t out = _pca_read(3);
    out &= ~(PCA_BIT_MODE | PCA_BIT_VCOMCTRL);
    out |= PCA_BIT_OE | PCA_BIT_WAKEUP;
    _pca_write(3, out);

    out |= PCA_BIT_PWRUP;
    _pca_write(3, out);

    lgfx::delay(1);
    lgfx::gpio_hi(_config.pin_spv);
    lgfx::gpio_hi(_config.pin_sph);

    lgfx::delay(20);

    _tps_write(TPS_REG_ENABLE, 0x3F);
    if (!_wait_tps_pwrgood(timeout_ms)) {
        _restore_after_vcom_measure(was_powered_on);
        return false;
    }

    (void)_tps_read(TPS_REG_INT1);
    _tps_write(TPS_REG_VCOM2, 0xA0);

    uint32_t start = lgfx::millis();
    do {
        uint8_t int1 = _tps_read(TPS_REG_INT1);
        if (int1 & 0x02) {
            uint8_t lsb = _tps_read(TPS_REG_VCOM1);
            uint8_t msb = _tps_read(TPS_REG_VCOM2);
            uint32_t raw = (lsb | (msb << 8)) & 0x1FF;
            *vcom_mV = raw * 10;
            ESP_LOGI(TAG, "Measured VCOM kick-back: raw=%lu, vcom=%lu mV", (unsigned long)raw, (unsigned long)*vcom_mV);
            _restore_after_vcom_measure(was_powered_on);
            return true;
        }
        lgfx::delay(1);
    } while ((lgfx::millis() - start) < timeout_ms);

    ESP_LOGE(TAG, "VCOM measurement timeout! INT1=%02X VCOM2=%02X",
             _tps_read(TPS_REG_INT1), _tps_read(TPS_REG_VCOM2));
    _restore_after_vcom_measure(was_powered_on);
    return false;
}

// ─── Helper stubs (delegate to static functions) ──────────────────

#define PCA_PORT _pca_port
#define PCA_ADDR _pca_addr
#define TPS_PORT _tps_port
#define TPS_ADDR _tps_addr

void Bus_EPD_V7::_pca_write(uint8_t reg, uint8_t val) {
    // Port 1 is shared with the frontlight enable signal.  EPD power
    // sequencing historically wrote a complete port image, which cleared
    // IO1_2 after any display refresh and turned an active frontlight off.
    // Preserve that independently-owned bit for all runtime EPD writes.
    if (reg == 3) {
        const uint8_t current = _pca_read(3);
        val = (uint8_t)((val & ~PCA_BIT_FRONTLIGHT)
                        | (current & PCA_BIT_FRONTLIGHT));
    }
    auto result = i2c::writeRegister8(PCA_PORT, PCA_ADDR, reg, val, 0x00, 100000);
    if (!result.has_value()) {
        ESP_LOGE(TAG, "PCA write failed: addr=0x%02X reg=0x%02X value=0x%02X",
                 PCA_ADDR, reg, val);
    }
}
uint8_t Bus_EPD_V7::_pca_read(uint8_t reg) {
    auto r = i2c::readRegister8(PCA_PORT, PCA_ADDR, reg, 100000);
    if (!r.has_value()) {
        ESP_LOGE(TAG, "PCA read failed: addr=0x%02X reg=0x%02X", PCA_ADDR, reg);
        return 0;
    }
    return r.value();
}
void Bus_EPD_V7::_pca_pin_hi(uint8_t bit) {
    _pca_write(3, (uint8_t)(_pca_read(3) | bit));
}
void Bus_EPD_V7::_pca_pin_lo(uint8_t bit) {
    _pca_write(3, (uint8_t)(_pca_read(3) & ~bit));
}
void Bus_EPD_V7::_tps_write(uint8_t reg, uint8_t val) {
    auto result = i2c::writeRegister8(TPS_PORT, TPS_ADDR, reg, val, 0x00, 100000);
    if (!result.has_value()) {
        ESP_LOGE(TAG, "TPS write failed: addr=0x%02X reg=0x%02X value=0x%02X",
                 TPS_ADDR, reg, val);
    }
}
uint8_t Bus_EPD_V7::_tps_read(uint8_t reg) {
    auto r = i2c::readRegister8(TPS_PORT, TPS_ADDR, reg, 100000);
    if (!r.has_value()) {
        ESP_LOGE(TAG, "TPS read failed: addr=0x%02X reg=0x%02X", TPS_ADDR, reg);
        return 0;
    }
    return r.value();
}
void Bus_EPD_V7::_tps_set_vcom(unsigned vcom_mV) {
    unsigned val = vcom_mV / 10;
    _tps_write(TPS_REG_VCOM2, (val & 0x100) >> 8);
    _tps_write(TPS_REG_VCOM1, val & 0xFF);
}
void Bus_EPD_V7::_tps_set_upseq_carta1300(void) {
    _tps_write(TPS_REG_UPSEQ0, 0xE1);
    _tps_write(TPS_REG_UPSEQ1, 0xAA);
}
bool Bus_EPD_V7::_wait_tps_pwrgood(uint32_t timeout_ms) {
    uint32_t start = lgfx::millis();
    do {
        uint8_t pg = _tps_read(TPS_REG_PG);
        if ((pg & 0xFA) == 0xFA) {
            return true;
        }
        lgfx::delay(1);
    } while ((lgfx::millis() - start) < timeout_ms);

    ESP_LOGE(TAG, "TPS65185 PG timeout! PG=%02X", _tps_read(TPS_REG_PG));
    return false;
}
void Bus_EPD_V7::_restore_after_vcom_measure(bool was_powered_on) {
    if (was_powered_on) {
        uint8_t out = _pca_read(3);
        out |= PCA_BIT_VCOMCTRL;
        _pca_write(3, out);
        _tps_set_vcom(_vcom_mV);
        _pwr_on = true;
    } else {
        _pwr_on = true;
        powerControl(false);
    }
}
#undef PCA_PORT
#undef PCA_ADDR
#undef TPS_PORT
#undef TPS_ADDR

// ─── PCA9535 Key Input (Port 0: IO0_0 ~ IO0_7) ─────────────────────

void IRAM_ATTR Bus_EPD_V7::_key_isr(void *arg) {
    SemaphoreHandle_t sem = (SemaphoreHandle_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void Bus_EPD_V7::setupKeyInput(int int_pin) {
    _key_int_pin = int_pin;

    // ── Configure PCA9535 Port 0 as all inputs ────────────────────
    uint8_t cfg0 = _pca_read(6);   // Config Port 0 reg
    cfg0 |= 0xFF;                  // all bits = input
    _pca_write(6, cfg0);

    // ── Create binary semaphore for ISR notification ──────────────
    if (!_key_sem) {
        _key_sem = xSemaphoreCreateBinary();
    }

    // ── GPIO ISR on INT pin (falling edge: idle high → low on key)
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_NEGEDGE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << int_pin);
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;   // idle = high
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add((gpio_num_t)int_pin, _key_isr, (void*)_key_sem);
}

uint8_t Bus_EPD_V7::scanKeys(void) {
    // Read PCA9535 Input Port 0 directly via I2C.
    // Returns ~key_state: 1 = pressed, 0 = released.
    uint8_t raw = _pca_read(0);
    return ~raw;   // invert: now bit=1 means pressed
}

int Bus_EPD_V7::getPressedKey(TickType_t timeout_ms) {
    if (!_key_sem) return -1;

    // Wait for INT falling edge notification
    if (xSemaphoreTake(_key_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;  // timeout
    }

    // Debounce: read, wait, re-read
    uint8_t first = _pca_read(0);
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t second = _pca_read(0);
    if (first != second) return -1;

    // Find first pressed key (0 = active low)
    for (int i = 0; i < 8; i++) {
        if (!(first & (1 << i))) {
            return i;   // key i pressed
        }
    }
    return -1;
}

} }
#endif
