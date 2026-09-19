#pragma once

#include "Bus_EPD.h"

namespace lgfx
{
 inline namespace v1
 {

  bool epd2d_continuous_scan_init(const Bus_EPD::config_t& config,
                                  size_t panel_width, size_t panel_height);
  // The waveform phase data is independent of the panel scan period.  This
  // diagnostic-only control changes the physical duration of one full-panel
  // phase between completed scans, so timing experiments do not require a
  // firmware rebuild or alter any LUT bytes.
  bool epd2d_continuous_scan_set_line_period_us(uint32_t line_period_us);
  uint32_t epd2d_continuous_scan_get_line_period_us();
  bool epd2d_continuous_scan_frame(const uint8_t* data, size_t length,
                                   uint32_t timeout_ms,
                                   uint32_t* scan_time_us);
  bool epd2d_continuous_scan_stream(
      Bus_EPD::epd_2d_line_renderer_t renderer, void* context,
      uint32_t timeout_ms, Bus_EPD::epd_2d_stream_stats_t* stats);

 }
}
