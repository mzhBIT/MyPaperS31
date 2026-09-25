#pragma once

#include "Panel_EPD.hpp"
#include "EpdWaveform2D.h"

#if SOC_LCD_I80_SUPPORTED

namespace lgfx
{
 inline namespace v1
 {

  /**
   * Differential 16-gray EPD engine.
   *
   * Drawing and framebuffer conversion are inherited from Panel_EPD.  This
   * class owns an independent refresh queue/task and never starts or mutates
   * the legacy one-dimensional waveform engine.
   */
  struct Panel_EPD_2D : public Panel_EPD
  {
    Panel_EPD_2D(void);
    ~Panel_EPD_2D(void) override;

    bool init(bool use_reset) override;
    void waitDisplay(void) override;
    bool displayBusy(void) override;
    void display(uint_fast16_t x, uint_fast16_t y,
                 uint_fast16_t w, uint_fast16_t h) override;
    bool writeNativeGrayscale4(const uint8_t* logical_frame,
                               uint16_t logical_width,
                               uint16_t logical_height) override;
    bool writeNativeGrayscale4Rect(const uint8_t* logical_frame,
                                   uint16_t logical_width,
                                   uint16_t logical_height,
                                   int16_t logical_x,
                                   int16_t logical_y) override;

    void waveform(const EpdWaveform* value) { _waveform = value; }
    const EpdWaveform* waveform(void) const { return _waveform; }
    void waveformTemperature(int celsius) { _temperature_c = celsius; }
    int waveformTemperature(void) const { return _temperature_c; }
    bool measureVCOM(uint32_t* vcom_mV, uint32_t timeout_ms = 500);

  private:
    struct update_data_t {
      uint16_t x;
      uint16_t y;
      uint16_t w;
      uint16_t h;
      epd_mode_t mode;
    };

    struct selected_waveform_t {
      const EpdWaveformPhases* phases = nullptr;
      uint8_t type = 0;
      uint8_t temperature_range = 0;
    };

    bool init_internal(void);
    selected_waveform_t selectWaveform(epd_mode_t mode) const;
    static void task_update(Panel_EPD_2D* me);

    Bus_EPD* getBusEPD2D(void) { return static_cast<Bus_EPD*>(_bus); }

    TaskHandle_t _task_update_2d = nullptr;
    QueueHandle_t _update_queue_2d = nullptr;
    uint8_t* _dma_bufs_2d[2] = { nullptr, nullptr };
    uint8_t* _scan_frame_2d = nullptr;
    size_t _scan_frame_bytes_2d = 0;
    uint8_t* _du4_phase_frames_2d = nullptr;
    size_t _du4_phase_frame_capacity_2d = 0;
    uint8_t* _du4_pair_lut_2d = nullptr;
    uint8_t* _gc16_pair_lut_2d = nullptr;
    uint16_t* _du4_active_groups_2d = nullptr;
    size_t _du4_active_group_capacity_2d = 0;
    uint8_t* _physical_buf = nullptr;
    uint8_t* _target_buf = nullptr;
    const EpdWaveform* _waveform = nullptr;
    volatile bool _display_busy_2d = false;
    volatile bool _physical_state_known = false;
    int _temperature_c = 25;
  };

 }
}

#endif
