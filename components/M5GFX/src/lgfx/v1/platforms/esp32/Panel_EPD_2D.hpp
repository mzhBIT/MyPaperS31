#pragma once

#include "Panel_EPD.hpp"
#include "EpdWaveform2D.h"
#include "EpdPageTurn.h"
#include "EpdPreparedStats.h"
#include <freertos/semphr.h>

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
    bool setPageTurnAnimation(int direction, int speed = 1) override;
    void display(uint_fast16_t x, uint_fast16_t y,
                 uint_fast16_t w, uint_fast16_t h) override;
    bool writeNativeGrayscale4(const uint8_t* logical_frame,
                               uint16_t logical_width,
                               uint16_t logical_height) override;
    bool writePhysicalGrayscale4(const uint8_t* physical_frame,
                                 uint16_t physical_width,
                                 uint16_t physical_height,
                                 uint8_t rotation) override;
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
    bool preparePhysicalGrayscale4Stats(const uint8_t* frame, uint16_t width,
                                        uint16_t height, uint8_t rotation,
                                        epd_mode_t mode) override;

  private:
    struct update_data_t {
      uint16_t x;
      uint16_t y;
      uint16_t w;
      uint16_t h;
      epd_mode_t mode;
      int8_t animation_direction;
      uint8_t animation_rotation;
      uint8_t animation_speed;
      int64_t queued_at_us;
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
    uint8_t* _du4_prime_pair_luts_2d = nullptr; // Two extra 4096-byte internal tables.
    uint8_t* _gc16_pair_lut_2d = nullptr;
    uint16_t* _du4_active_groups_2d = nullptr;
    size_t _du4_active_group_capacity_2d = 0;
    uint8_t* _physical_buf = nullptr;
    uint8_t* _target_buf = nullptr;
    SemaphoreHandle_t _stats_mutex = nullptr;
    uint64_t _physical_generation = 0;
    struct {
      bool valid = false;
      lgfx_epd_stats::PreparedKey key{};
      uint8_t* target = nullptr;
      uint16_t* groups = nullptr;
      size_t group_count = 0, transition_count = 0;
      bool used[256]{};
      bool fused = false;
    } _prepared_stats;
    const EpdWaveform* _waveform = nullptr;
    volatile bool _display_busy_2d = false;
    volatile bool _physical_state_known = false;
    int _temperature_c = 25;
    int8_t _next_animation_direction = 0;
    uint8_t _next_animation_speed = 1;
    // Lazy geometry-only caches. Owned solely by task_update, like DU4 workspace.
    // Panel lifetime is process lifetime; shutdown teardown remains passive.
    lgfx_epd_page_turn::Layout _animation_layouts_2d[8]{};
  };

 }
}

#endif
