#include "Panel_EPD_2D.hpp"
#include "EpdTransitionStats.h"
#include "EpdDu4Triple.h"
#include "EpdPairIndices.h"
#include "EpdGc16Dual.h"
#include "EpdChangedPixels.h"
#include "EpdScan2DContinuous.hpp"

#if SOC_LCD_I80_SUPPORTED && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31))

#include "lgfx/v1/platforms/common.hpp"
#include "lgfx/v1/platforms/esp32/MyPaper_EPD_Config.hpp"

#include <algorithm>
#include <cstring>
#include <climits>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_task.h>
#include <esp_timer.h>

#if __has_include(<esp_cache.h>)
#include <esp_cache.h>
#endif

extern "C" __attribute__((weak))
const EpdWaveform* lgfx_epd_2d_waveform_provider(void)
{
  return nullptr;
}

namespace lgfx
{
 inline namespace v1
 {

  static constexpr char TAG_2D[] = "Panel_EPD_2D";
  static constexpr uint8_t WAVEFORM_MODE_DU = 1;
  static constexpr uint8_t WAVEFORM_MODE_GC16 = 2;
  static constexpr uint8_t WAVEFORM_MODE_A2 = 4;
  static constexpr uint8_t WAVEFORM_MODE_GL16 = 5;
  static constexpr uint8_t WAVEFORM_MODE_DU4 = 7;
  static constexpr uint8_t WAVEFORM_MODE_HD = 8;

  // Temporary diagnostic, reported on every call rather than past a threshold.
  // The USB isochronous OUT pipe hands the next DMA buffer to the hardware from
  // its ISR, at the microframe the previous one ends, so anything that delays
  // that ISR by a fraction of a microframe shows up as "TX URB: 1/N packets
  // skipped, first=0". This writeback is the largest CPU-bound operation on the
  // display path, so its duration matters at every refresh. A threshold makes
  // "fast" and "not running here any more" look identical in the log, which is
  // exactly the ambiguity that cost a test round.
  static void cache_writeback_2d(const void* ptr, size_t size)
  {
#if defined(ESP_CACHE_MSYNC_FLAG_DIR_C2M)
    if (ptr != nullptr && size != 0 && !isEmbeddedMemory(ptr)) {
      uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~uintptr_t(127);
      uintptr_t end = (reinterpret_cast<uintptr_t>(ptr) + size + 127) & ~uintptr_t(127);
      if (start < end) {
        const int64_t t0 = esp_timer_get_time();
        constexpr size_t sync_chunk = 16 * 1024;
        uint32_t max_chunk_us = 0;
        for (uintptr_t cursor = start; cursor < end; cursor += sync_chunk) {
          const size_t chunk = std::min<size_t>(sync_chunk, end - cursor);
          const int64_t chunk_begin = esp_timer_get_time();
          esp_cache_msync(reinterpret_cast<void*>(cursor), chunk,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
          max_chunk_us = std::max<uint32_t>(max_chunk_us,
              static_cast<uint32_t>(esp_timer_get_time() - chunk_begin));
        }
        ESP_LOGD(TAG_2D, "cache writeback: %u bytes in %lld us max_chunk=%lu us",
                 static_cast<unsigned>(end - start),
                 static_cast<long long>(esp_timer_get_time() - t0),
                 static_cast<unsigned long>(max_chunk_us));
      }
    }
#else
    (void)ptr;
    (void)size;
#endif
  }

  static uint8_t waveform_drive(const uint8_t* phase_lut,
                                uint8_t from, uint8_t to)
  {
    const uint8_t packed = phase_lut[(to << 2) + (from >> 2)];
    return (packed >> (6 - ((from & 3) << 1))) & 3;
  }

  static void build_transition_lut(uint8_t* transition,
                                   const uint8_t* phase_lut)
  {
    for (unsigned from = 0; from < 16; ++from) {
      for (unsigned to = 0; to < 16; ++to) {
        transition[(from << 4) | to] = waveform_drive(phase_lut, from, to);
      }
    }
  }

  __attribute__((optimize("O3"))) static size_t collect_used_transitions_4bpp(
      bool* used, const uint8_t* physical, const uint8_t* target,
      size_t stride, size_t panel_height,
      uint16_t x, uint16_t y, uint16_t width, uint16_t height)
  {
    return lgfx_epd_stats::collect_rect(used, physical, target,
        stride, panel_height, x, y, width, height);
  }

  static bool transitions_equal_for_used(
      const uint8_t* lhs, const uint8_t* rhs, const bool* used)
  {
    for (size_t i = 0; i < 256; ++i) {
      if (used[i] && lhs[i] != rhs[i]) {
        return false;
      }
    }
    return true;
  }

  static uint8_t quantize_du4_gray(uint8_t gray)
  {
    if (gray <= 2) return 0;
    if (gray <= 7) return 5;
    if (gray <= 12) return 10;
    return 15;
  }

  static uint8_t quantize_a2_gray(uint8_t gray)
  {
    return gray < 8 ? 0 : 15;
  }

  __attribute__((optimize("O3"))) static void quantize_binary_target_rect_4bpp(
      uint8_t* target, size_t stride, size_t panel_width, size_t panel_height,
      uint16_t x, uint16_t y, uint16_t width, uint16_t height)
  {
    const size_t pixel_begin = std::min<size_t>(x, panel_width);
    const size_t pixel_end = std::min<size_t>(size_t(x) + width, panel_width);
    const size_t row_begin = std::min<size_t>(y, panel_height);
    const size_t row_end = std::min<size_t>(size_t(y) + height, panel_height);
    if (pixel_begin >= pixel_end || row_begin >= row_end) return;

    for (size_t row = row_begin; row < row_end; ++row) {
      uint8_t* pixels = target + row * stride;
      size_t begin = pixel_begin;
      if (begin & 1u) {
        const size_t index = begin >> 1;
        pixels[index] = uint8_t((pixels[index] & 0xF0u)
                              | quantize_a2_gray(pixels[index] & 0x0Fu));
        ++begin;
      }
      const size_t full_byte_end = pixel_end >> 1;
      for (size_t index = begin >> 1; index < full_byte_end; ++index) {
        const uint8_t pair = pixels[index];
        pixels[index] = uint8_t((pair & 0x80u ? 0xF0u : 0u)
                              | (pair & 0x08u ? 0x0Fu : 0u));
      }
      if (pixel_end & 1u) {
        const size_t index = pixel_end >> 1;
        pixels[index] = uint8_t((pixels[index] & 0x0Fu)
                              | (quantize_a2_gray(pixels[index] >> 4) << 4));
      }
    }
  }

  static void apply_gc16_transition_adjustments(
      uint8_t* transition, uint8_t waveform_type,
      int phase, int phase_count, bool changed_pixels_only = false)
  {
    if (changed_pixels_only) {
      lgfx_epd_changed_pixels::mask_unchanged(transition);
      return;
    }
    // Keep the 36-phase diagnostic trim byte-for-byte intact.
    if (waveform_type != WAVEFORM_MODE_GC16 || phase_count != 48) {
      return;
    }
#if MYPAPER_EPD_2D_GC16_00_REBALANCE
    // Original 0->0: idle 10, white 18, black 17, idle 3. Reassign the
    // final six white phases: idle 10, white 12, black 23, idle 3.
    if (phase >= 22 && phase <= 27) {
      transition[0x00] = 1;
    }
#endif
#if MYPAPER_EPD_2D_GC16_01_REBALANCE
    // Original 0->1: idle 9, white 18, black 17, white 1, idle 3.
    // Move the last six phases of its first white segment to black.
    if (phase >= 21 && phase <= 26) {
      transition[0x01] = 1;
    }
#endif
#if MYPAPER_EPD_2D_GC16_02_REBALANCE
    // Original 0->2: idle 6, white 19, black 17, white 1, black 1,
    // white 1, idle 3. Move phases 22..24 to black; leave the tail intact.
    if (phase >= 22 && phase <= 24) {
      transition[0x02] = 1;
    }
#endif
#if MYPAPER_EPD_2D_GC16_03_REBALANCE
    // Original 0->3 phase 24 is the end of its first white-drive segment.
    if (phase == 24) {
      transition[0x03] = 1;
    }
#endif
#if MYPAPER_EPD_2D_GC16_04_REBALANCE
    // Original 0->4 phase 23 is the end of its first white-drive segment.
    if (phase == 23) {
      transition[0x04] = 1;
    }
#endif
#if MYPAPER_EPD_2D_GC16_07_REBALANCE
    // Original 0->7 phases 23..25 begin its first black-drive segment.
    if (phase >= 23 && phase <= 25) {
      transition[0x07] = 2;
    }
#endif
#if MYPAPER_EPD_2D_GC16_08_REBALANCE
    // Original 0->8 phases 23..24 begin its black-drive segment.
    if (phase >= 23 && phase <= 24) {
      transition[0x08] = 2;
    }
#endif
#if MYPAPER_EPD_2D_GC16_09_REBALANCE
    // Original 0->9 phases 23..24 begin its first black-drive segment.
    if (phase >= 23 && phase <= 24) {
      transition[0x09] = 2;
    }
#endif
#if MYPAPER_EPD_2D_GC16_0A_REBALANCE
    // Original 0->10 phases 21..23 begin its black-drive segment.
    if (phase >= 21 && phase <= 23) {
      transition[0x0A] = 2;
    }
#endif
#if MYPAPER_EPD_2D_GC16_0B_REBALANCE
    // Original 0->11 phases 19..22 begin its first black-drive segment.
    if (phase >= 19 && phase <= 22) {
      transition[0x0B] = 2;
    }
#endif
#if MYPAPER_EPD_2D_GC16_0C_REBALANCE
    // Original 0->12 phases 21..24 begin its black-drive segment.
    if (phase >= 21 && phase <= 24) {
      transition[0x0C] = 2;
    }
#endif
#if MYPAPER_EPD_2D_GC16_0D_REBALANCE
    // Original 0->13 phases 19..20 begin its first black-drive segment.
    if (phase >= 19 && phase <= 20) {
      transition[0x0D] = 2;
    }
#endif
#if MYPAPER_EPD_2D_GC16_0E_REBALANCE
    // Original 0->14 phase 18 begins its black-drive segment.
    if (phase == 18) {
      transition[0x0E] = 2;
    }
#endif
#if MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES > 0
    // Preserve the final phase as an all-idle settling frame.  Consume up to
    // the configured number of phases immediately before it.  Index 0x00 is
    // exactly old-gray 0 -> new-gray 0; drive value 1 is black-direction.
    const int available = phase_count - 1;
    const int count = std::min<int>(
        MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES, available);
    const int first = phase_count - 1 - count;
    if (phase >= first && phase < phase_count - 1) {
      transition[0x00] = 1;
    }
#endif
  }

  __attribute__((always_inline)) static inline uint8_t
  drive_pixel_pair(uint8_t from, uint8_t to, const uint8_t* transition)
  {
    const uint8_t high = transition[(from & 0xF0) | (to >> 4)];
    const uint8_t low = transition[((from & 0x0F) << 4) | (to & 0x0F)];
    return (high << 2) | low;
  }

  static constexpr size_t DU4_PAIR_LUT_BYTES = 256u * 16u;
  static constexpr size_t DU4_TARGET_CODE_LUT_BYTES = 256u;
  static constexpr size_t DU4_RENDER_LUT_BYTES =
      DU4_PAIR_LUT_BYTES + DU4_TARGET_CODE_LUT_BYTES;
  static constexpr size_t GC16_PAIR_LUT_BYTES = 256u * 256u;

  __attribute__((optimize("O3"))) static void build_gc16_pair_lut(
      uint8_t* pair_lut, const uint8_t* transition)
  {
    uint8_t high_drive[16][16];
    uint32_t low_drive_words[16][4];

    // A byte pair is separable: the high old/target nibbles produce the upper
    // two drive bits and the low nibbles produce the lower two.  Resolve the
    // 512 transition reads once instead of repeating two reads for every one
    // of the 65,536 pair-LUT entries.
    for (unsigned old_gray = 0; old_gray < 16; ++old_gray) {
      for (unsigned target_gray = 0; target_gray < 16; ++target_gray) {
        high_drive[old_gray][target_gray] =
            uint8_t(transition[(old_gray << 4) | target_gray] << 2);
      }
      for (unsigned block = 0; block < 4; ++block) {
        uint32_t packed = 0;
        for (unsigned lane = 0; lane < 4; ++lane) {
          const unsigned target_gray = (block << 2) | lane;
          packed |= uint32_t(
              transition[(old_gray << 4) | target_gray]) << (lane << 3);
        }
        low_drive_words[old_gray][block] = packed;
      }
    }

    typedef uint32_t alias_uint32_t __attribute__((may_alias));
    for (unsigned from_pair = 0; from_pair < 256; ++from_pair) {
      const unsigned old_high = from_pair >> 4;
      const unsigned old_low = from_pair & 0x0F;
      auto* row = reinterpret_cast<alias_uint32_t*>(
          pair_lut + (from_pair << 8));
      for (unsigned target_high = 0; target_high < 16; ++target_high) {
        const uint32_t high_word =
            uint32_t(high_drive[old_high][target_high]) * 0x01010101u;
        const unsigned word_base = target_high << 2;
        row[word_base + 0] = high_word | low_drive_words[old_low][0];
        row[word_base + 1] = high_word | low_drive_words[old_low][1];
        row[word_base + 2] = high_word | low_drive_words[old_low][2];
        row[word_base + 3] = high_word | low_drive_words[old_low][3];
      }
    }
  }

  __attribute__((always_inline)) static inline uint8_t
  du4_bucket_index(uint8_t gray)
  {
    if (gray <= 2) return 0;
    if (gray <= 7) return 1;
    if (gray <= 12) return 2;
    return 3;
  }

  static void build_du4_pair_lut(uint8_t* pair_lut,
                                 const uint8_t* transition)
  {
    static constexpr uint8_t bucket_gray[4] = { 0, 5, 10, 15 };
    for (unsigned from_pair = 0; from_pair < 256; ++from_pair) {
      uint8_t* row = pair_lut + (from_pair << 4);
      for (unsigned target_code = 0; target_code < 16; ++target_code) {
        const uint8_t target_pair = uint8_t(
            (bucket_gray[target_code >> 2] << 4)
          | bucket_gray[target_code & 3]);
        row[target_code] = drive_pixel_pair(
            uint8_t(from_pair), target_pair, transition);
      }
    }
  }

  static void build_du4_target_code_lut(uint8_t* target_code_lut)
  {
    for (unsigned pair = 0; pair < 256; ++pair) {
      target_code_lut[pair] = uint8_t(
          (du4_bucket_index(uint8_t(pair >> 4)) << 2)
        | du4_bucket_index(uint8_t(pair & 0x0F)));
    }
  }

  static bool du4_equal_pairs_are_idle(const uint8_t* transition)
  {
    for (unsigned gray = 0; gray < 16; ++gray) {
      if (transition[(gray << 4) | gray] != 0) {
        return false;
      }
    }
    return true;
  }

  static void build_driven_transition_mask(
      bool* driven, const EpdWaveformPhases* phases, uint8_t waveform_type)
  {
    memset(driven, 0, 256 * sizeof(*driven));
    uint8_t transition[256];
    for (int phase = 0; phase < phases->phases; ++phase) {
      build_transition_lut(
          transition, phases->luts + size_t(phase) * 64);
      apply_gc16_transition_adjustments(
          transition, waveform_type, phase, phases->phases);
      for (size_t i = 0; i < 256; ++i) {
        driven[i] |= transition[i] != 0;
      }
    }
  }

  __attribute__((optimize("O3"))) static size_t collect_active_groups_fullscreen(
      uint16_t* active_groups, size_t capacity,
      const uint8_t* physical, const uint8_t* target,
      size_t stride, size_t panel_height, const bool* driven)
  {
    const size_t groups_per_row = stride >> 3;
    const size_t total_groups = groups_per_row * panel_height;
    if (active_groups == nullptr || capacity < total_groups
     || total_groups > UINT16_MAX) {
      return total_groups;
    }

    size_t count = 0;
    for (size_t group_index = 0; group_index < total_groups; ++group_index) {
      const size_t base = group_index << 3;
      bool active = false;
      for (unsigned pair = 0; pair < 8; ++pair) {
        const uint8_t from = physical[base + pair];
        const uint8_t to = target[base + pair];
        const uint8_t high = (from & 0xF0) | (to >> 4);
        const uint8_t low = ((from & 0x0F) << 4) | (to & 0x0F);
        if (driven[high] || driven[low]) {
          active = true;
          break;
        }
      }
      if (active) {
        active_groups[count++] = uint16_t(group_index);
      }
    }
    return count;
  }

  __attribute__((always_inline)) static inline uint32_t
  permute_epd_output(uint32_t value)
  {
#if MYPAPER_EPD_OUTPUT_PERMUTE == 1
    return ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
#elif MYPAPER_EPD_OUTPUT_PERMUTE == 0
    return value;
#else
#error "Unsupported MYPAPER_EPD_OUTPUT_PERMUTE"
#endif
  }

  // Convert one complete waveform phase while the panel is still powered off.
  // Quality refreshes use the branch-free full-screen path.  Keeping this as
  // an O3 function matters because the component debug build otherwise uses
  // -Og and leaves large gaps between physical waveform phases.
  __attribute__((optimize("O3"))) static void render_phase_frame(
      uint8_t* output_frame, size_t output_bytes,
      const uint8_t* physical, const uint8_t* target,
      size_t panel_width, size_t panel_height,
      size_t memory_width, size_t scan_rows,
      uint16_t update_x, uint16_t update_y,
      uint16_t update_width, uint16_t update_height,
      const uint8_t* transition)
  {
    static constexpr uint8_t pair_shifts[8] = {
      4, 0, 12, 8, 20, 16, 28, 24
    };
    const size_t stride = panel_width >> 1;
    const size_t groups_per_row = memory_width >> 4;
    const size_t output_stride = memory_width >> 2;
    const bool full_screen = update_x == 0 && update_y == 0
                          && update_width == panel_width
                          && update_height == panel_height
                          && scan_rows == panel_height;

    if (full_screen) {
      for (size_t y = 0; y < scan_rows; ++y) {
        uint32_t* output = reinterpret_cast<uint32_t*>(
            output_frame + y * output_stride);
        const uint8_t* from_row = physical + y * stride;
        const uint8_t* to_row = target + y * stride;
        for (size_t group = 0; group < groups_per_row; ++group) {
          uint32_t packed = 0;
          const size_t base = group << 3;
          for (unsigned pair = 0; pair < 8; ++pair) {
            packed |= uint32_t(drive_pixel_pair(
                from_row[base + pair], to_row[base + pair], transition))
                      << pair_shifts[pair];
          }
          output[group] = permute_epd_output(packed);
        }
      }
      return;
    }

    // DMA still scans a complete panel frame, so untouched positions must be
    // idle.  Clear them once, then visit only rows and 16-pixel groups that
    // intersect the requested update rectangle.  The old implementation
    // walked the complete frame and performed a bounds test for every pair,
    // making a partial update nearly as expensive to prepare as a full one.
    memset(output_frame, 0, output_bytes);
    const size_t byte_begin = std::min<size_t>(update_x >> 1, stride);
    const size_t byte_end = std::min<size_t>(
        (size_t(update_x) + update_width) >> 1, stride);
    const size_t row_begin = std::min<size_t>(update_y,
                                              std::min(panel_height, scan_rows));
    const size_t row_end = std::min<size_t>(
        size_t(update_y) + update_height, std::min(panel_height, scan_rows));
    const size_t group_begin = byte_begin >> 3;
    const size_t group_end = std::min<size_t>(
        (byte_end + 7) >> 3, groups_per_row);

    for (size_t y = row_begin; y < row_end; ++y) {
      uint32_t* output = reinterpret_cast<uint32_t*>(
          output_frame + y * output_stride);
      const uint8_t* from_row = physical + y * stride;
      const uint8_t* to_row = target + y * stride;
      for (size_t group = group_begin; group < group_end; ++group) {
        uint32_t packed = 0;
        const size_t base = group << 3;
        if (base >= byte_begin && base + 8 <= byte_end
         && base + 8 <= stride) {
          for (unsigned pair = 0; pair < 8; ++pair) {
            packed |= uint32_t(drive_pixel_pair(
                from_row[base + pair], to_row[base + pair], transition))
                      << pair_shifts[pair];
          }
        } else {
          // At most the first and last intersecting groups need clipping.
          for (unsigned pair = 0; pair < 8; ++pair) {
            const size_t index = base + pair;
            if (index >= byte_begin && index < byte_end && index < stride) {
              packed |= uint32_t(drive_pixel_pair(
                  from_row[index], to_row[index], transition))
                        << pair_shifts[pair];
            }
          }
        }
        output[group] = permute_epd_output(packed);
      }
    }
  }

  // Batch-owned diagnostics: no logging or per-pixel timers on the scan path.
  struct drive_frame_timing_t {
    uint64_t lut_us = 0, clear_us = 0, pixels_us = 0, total_us = 0;
    unsigned count = 1; // Fused rendering is measured as a group, never divided artificially.
    int phase = -1;
    bool prime = false;
  };

  // GC16 Quality always scans the complete panel.  A 64 KiB pair LUT maps
  // the two old 4bpp pixels and two target 4bpp pixels directly to their two
  // 2-bit drive values, removing the hot loop's masks and dual transition
  // lookups without changing a single waveform output bit.
  __attribute__((optimize("O3"))) static void render_gc16_pair_phase_frame(
      uint8_t* output_frame,
      const uint8_t* physical, const uint8_t* target,
      size_t memory_width, size_t scan_rows,
      const uint8_t* pair_lut,
      uint16_t update_x, uint16_t update_y,
      uint16_t update_width, uint16_t update_height,
      drive_frame_timing_t* timing = nullptr,
      const uint16_t* indices = nullptr)
  {
    constexpr bool swap_bytes = MYPAPER_EPD_OUTPUT_PERMUTE == 1;
    const auto& pair_shifts = lgfx_epd_pair_indices::output_shifts<swap_bytes>;
    const size_t stride = memory_width >> 1;
    const size_t groups_per_row = memory_width >> 4;
    const size_t output_stride = memory_width >> 2;

    const int64_t pixels_begin = timing ? esp_timer_get_time() : 0;
    // Outside a partial update every source must remain neutral, including
    // pixels sharing a 16-pixel output word with the requested rectangle.
    if (update_x != 0 || update_y != 0
     || update_width != memory_width || update_height != scan_rows) {
      memset(output_frame, 0, output_stride * scan_rows);
      const int64_t clear_end = timing ? esp_timer_get_time() : 0;
      if (timing) timing->clear_us = clear_end - pixels_begin;
      const size_t byte_begin = update_x >> 1;
      const size_t byte_end = std::min(stride, size_t(update_x + update_width) >> 1);
      const size_t row_end = std::min(scan_rows, size_t(update_y + update_height));
      for (size_t y = update_y; y < row_end; ++y) {
        auto* output = reinterpret_cast<uint32_t*>(output_frame + y * output_stride);
        const uint8_t* from_row = physical + y * stride;
        const uint8_t* to_row = target + y * stride;
        for (size_t group = byte_begin >> 3; group < (byte_end + 7) >> 3; ++group) {
          const size_t base = group << 3;
          uint32_t packed = 0;
          if (indices) {
            const auto* index_group = indices + y * stride + base;
            if (base >= byte_begin && base + 8 <= byte_end) {
              packed = lgfx_epd_pair_indices::group<swap_bytes>(index_group, pair_lut);
            } else {
              packed = lgfx_epd_pair_indices::group_range<swap_bytes>(index_group, pair_lut,
                  unsigned(std::max(base, byte_begin) - base),
                  unsigned(std::min(base + 8, byte_end) - base));
            }
          } else if (base >= byte_begin && base + 8 <= byte_end) {
            // Interior groups use the same branch-free hot loop as full-screen.
            for (unsigned pair = 0; pair < 8; ++pair) {
              const unsigned index = (unsigned(from_row[base + pair]) << 8)
                                   | unsigned(to_row[base + pair]);
              packed |= uint32_t(pair_lut[index]) << pair_shifts[pair];
            }
          } else {
            const unsigned first = unsigned(std::max(base, byte_begin) - base);
            const unsigned end = unsigned(std::min(base + 8, byte_end) - base);
            for (unsigned pair = first; pair < end; ++pair) {
              const unsigned index = (unsigned(from_row[base + pair]) << 8)
                                   | unsigned(to_row[base + pair]);
              packed |= uint32_t(pair_lut[index]) << pair_shifts[pair];
            }
          }
          output[group] = packed;
        }
      }
      if (timing) timing->pixels_us = esp_timer_get_time() - clear_end;
      return;
    }

    if (indices) {
      for (size_t y = 0; y < scan_rows; ++y) {
        auto* output = reinterpret_cast<uint32_t*>(output_frame + y * output_stride);
        const uint16_t* index_row = indices + y * stride;
        for (size_t group = 0; group < groups_per_row; ++group)
          output[group] = lgfx_epd_pair_indices::group<swap_bytes>(
              index_row + (group << 3), pair_lut);
      }
      if (timing) timing->pixels_us = esp_timer_get_time() - pixels_begin;
      return;
    }

    for (size_t y = 0; y < scan_rows; ++y) {
      uint32_t* output = reinterpret_cast<uint32_t*>(
          output_frame + y * output_stride);
      const uint8_t* from_row = physical + y * stride;
      const uint8_t* to_row = target + y * stride;
      for (size_t group = 0; group < groups_per_row; ++group) {
        const size_t base = group << 3;
        uint32_t packed = 0;
        for (unsigned pair = 0; pair < 8; ++pair) {
          const unsigned index = (unsigned(from_row[base + pair]) << 8)
                               | unsigned(to_row[base + pair]);
          packed |= uint32_t(pair_lut[index]) << pair_shifts[pair];
        }
        output[group] = packed;
      }
    }
    if (timing) timing->pixels_us = esp_timer_get_time() - pixels_begin;
  }

  static constexpr int GC16_PIPELINE_MAX_PHASES = 64;
  static constexpr int GC16_PIPELINE_MAX_SLOTS = 20;

  static BaseType_t create_epd_pipeline_task(
      TaskFunction_t entry, const char* name, uint32_t stack_size,
      void* argument, UBaseType_t priority, TaskHandle_t* handle,
      BaseType_t core, bool* external_stack)
  {
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    *external_stack = true;
    const BaseType_t external_result = xTaskCreatePinnedToCoreWithCaps(
        entry, name, stack_size, argument, priority, handle, core,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (external_result == pdPASS) {
      return external_result;
    }
    ESP_LOGW(TAG_2D,
             "%s PSRAM stack allocation failed; trying internal RAM", name);
#endif
    *external_stack = false;
    return xTaskCreatePinnedToCore(entry, name, stack_size, argument,
                                   priority, handle, core);
  }

  struct gc16_phase_pipeline_t
  {
    drive_frame_timing_t* timings = nullptr;
    const uint16_t* indices = nullptr;
    uint8_t* frames = nullptr;
    size_t frame_bytes = 0;
    const uint8_t* physical = nullptr;
    const uint8_t* target = nullptr;
    uint8_t* pair_lut = nullptr;
    const uint8_t* unique_transitions = nullptr;
    size_t memory_width = 0;
    size_t scan_rows = 0;
    uint16_t update_x = 0, update_y = 0, update_width = 0, update_height = 0;
    int unique_count = 0;
    int slot_count = 0;
    int next_unique = 0;
    volatile int consumed_phase = -1;
    volatile int ready_unique[GC16_PIPELINE_MAX_SLOTS] = {};
    volatile int unique_slot[GC16_PIPELINE_MAX_PHASES] = {};
    uint16_t last_phase[GC16_PIPELINE_MAX_PHASES] = {};
    volatile bool abort = false;
    volatile bool finished = false;
    volatile bool failed = false;
    bool external_stack = false;
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t done = nullptr;
    uint64_t lut_build_us = 0;
    uint64_t frame_render_us = 0;
    int64_t producer_started_us = 0;
    int64_t producer_finished_us = 0;
  };

  static void render_gc16_pipeline_unique(gc16_phase_pipeline_t* ctx,
                                          int unique, int slot)
  {
    const uint8_t* transition =
        ctx->unique_transitions + size_t(unique) * 256;
    const int64_t lut_begin = esp_timer_get_time();
    build_gc16_pair_lut(ctx->pair_lut, transition);
    const int64_t render_begin = esp_timer_get_time();
    ctx->lut_build_us += render_begin - lut_begin;
    auto* timing = ctx->timings ? &ctx->timings[unique] : nullptr;
    if (timing) timing->lut_us = render_begin - lut_begin;
    render_gc16_pair_phase_frame(
        ctx->frames + size_t(slot) * ctx->frame_bytes,
        ctx->physical, ctx->target,
        ctx->memory_width, ctx->scan_rows, ctx->pair_lut,
        ctx->update_x, ctx->update_y, ctx->update_width, ctx->update_height,
        timing, ctx->indices);
    ctx->frame_render_us += esp_timer_get_time() - render_begin;
    if (timing) timing->total_us = esp_timer_get_time() - lut_begin;
    __atomic_store_n(&ctx->unique_slot[unique], slot, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->ready_unique[slot], unique, __ATOMIC_RELEASE);
  }

  static int claim_gc16_pipeline_slot(gc16_phase_pipeline_t* ctx, int excluded = -1)
  {
    const int consumed = __atomic_load_n(&ctx->consumed_phase, __ATOMIC_ACQUIRE);
    const int slot = lgfx_epd_gc16_dual::reusable_slot(
        ctx->slot_count, consumed, ctx->last_phase, [ctx](int candidate) {
          return __atomic_load_n(&ctx->ready_unique[candidate], __ATOMIC_ACQUIRE);
        }, excluded);
    if (slot >= 0) {
      const int resident = __atomic_load_n(&ctx->ready_unique[slot], __ATOMIC_ACQUIRE);
      if (resident >= 0)
        __atomic_store_n(&ctx->unique_slot[resident], -1, __ATOMIC_RELEASE);
      __atomic_store_n(&ctx->ready_unique[slot], -1, __ATOMIC_RELEASE);
    }
    return slot;
  }

#if MYPAPER_EPD_2D_GC16_DUAL_WORKER
  static void render_gc16_pipeline_pair(gc16_phase_pipeline_t* ctx,
                                        int unique, int first_slot, int second_slot)
  {
    const int64_t begin = esp_timer_get_time();
    lgfx_epd_gc16_dual::build_lut(ctx->pair_lut,
        ctx->unique_transitions + size_t(unique) * 256,
        ctx->unique_transitions + size_t(unique + 1) * 256);
    const int64_t lut_end = esp_timer_get_time();
    ctx->lut_build_us += lut_end - begin;
    auto* first = ctx->frames + size_t(first_slot) * ctx->frame_bytes;
    auto* second = ctx->frames + size_t(second_slot) * ctx->frame_bytes;
    const bool partial = ctx->update_x != 0 || ctx->update_y != 0
        || ctx->update_width != ctx->memory_width || ctx->update_height != ctx->scan_rows;
    if (partial) {
      memset(first, 0, ctx->frame_bytes);
      memset(second, 0, ctx->frame_bytes);
    }
    const int64_t clear_end = partial ? esp_timer_get_time() : lut_end;
    lgfx_epd_gc16_dual::render<MYPAPER_EPD_OUTPUT_PERMUTE == 1,true>(
        first, second, ctx->physical, ctx->target, ctx->indices, ctx->pair_lut,
        ctx->memory_width, ctx->scan_rows,
        ctx->update_x, ctx->update_y, ctx->update_width, ctx->update_height, true);
    const int64_t end = esp_timer_get_time();
    ctx->frame_render_us += end - lut_end;
    if (ctx->timings) {
      auto& detail = ctx->timings[unique];
      detail.lut_us = lut_end - begin;
      detail.clear_us = clear_end - lut_end;
      detail.pixels_us = end - clear_end;
      detail.total_us = end - begin;
      detail.count = 2;
      detail.prime = false;
    }
    // Both outputs are complete before either mapping becomes visible. The
    // slots need not be adjacent, and neither can be reused by this producer.
    __atomic_store_n(&ctx->unique_slot[unique], first_slot, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->ready_unique[first_slot], unique, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->unique_slot[unique + 1], second_slot, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->ready_unique[second_slot], unique + 1, __ATOMIC_RELEASE);
  }
#endif

  static void gc16_phase_producer_task(void* argument)
  {
    auto* ctx = static_cast<gc16_phase_pipeline_t*>(argument);
    ctx->producer_started_us = esp_timer_get_time();
    for (int unique = ctx->next_unique;
         unique < ctx->unique_count && !ctx->abort;) {
      int slot = -1;
      while (!ctx->abort && slot < 0) {
        slot = claim_gc16_pipeline_slot(ctx);
        if (slot < 0) {
          ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
      }
      if (ctx->abort) {
        break;
      }
      int count = 1;
#if MYPAPER_EPD_2D_GC16_DUAL_WORKER
      if (ctx->indices && unique + 1 < ctx->unique_count) {
        // Never wait for a second slot: delaying the next required frame could
        // cause an underrun. With only one free slot use the original path.
        const int second_slot = claim_gc16_pipeline_slot(ctx, slot);
        if (second_slot >= 0) {
          render_gc16_pipeline_pair(ctx, unique, slot, second_slot);
          count = 2;
        }
      }
#endif
      if (count == 1) render_gc16_pipeline_unique(ctx, unique, slot);
      unique += count;
      ctx->next_unique = unique;
    }
    const bool external_stack = ctx->external_stack;
    ctx->producer_finished_us = esp_timer_get_time();
    ctx->finished = true;
    xSemaphoreGive(ctx->done);
    if (external_stack) {
      vTaskDeleteWithCaps(nullptr);
    } else {
      vTaskDelete(nullptr);
    }
  }

  static void finish_gc16_phase_pipeline(gc16_phase_pipeline_t* ctx,
                                         bool abort)
  {
    if (ctx->done == nullptr) {
      return;
    }
    if (abort) {
      ctx->abort = true;
    }
    if (!ctx->finished && ctx->task != nullptr) {
      xTaskNotifyGive(ctx->task);
    }
    xSemaphoreTake(ctx->done, portMAX_DELAY);
    vSemaphoreDelete(ctx->done);
    ctx->done = nullptr;
    ctx->task = nullptr;
  }

  static inline void du4_bus_relief()
  {
#if MYPAPER_EPD_2D_DU4_BUS_RELIEF_US > 0
    // Both CPUs share the external-memory cache/bus.  A short ROM-only pause
    // lets the core-0 UAC2 event path fetch code and resubmit its isochronous
    // URB without making the EPD task sleep for a complete RTOS tick.
    esp_rom_delay_us(MYPAPER_EPD_2D_DU4_BUS_RELIEF_US);
#endif
  }

  static void clear_du4_frame(uint8_t* output_frame, size_t output_bytes)
  {
    size_t offset = 0;
    while (offset < output_bytes) {
      const size_t chunk = std::min<size_t>(
          MYPAPER_EPD_2D_DU4_BUS_BURST_BYTES, output_bytes - offset);
      memset(output_frame + offset, 0, chunk);
      offset += chunk;
      if (offset < output_bytes) {
        du4_bus_relief();
      }
    }
  }

  struct du4_render_timing_t {
    uint64_t clear_us = 0;
    uint64_t pixels_us = 0;
  };

  template<unsigned Count, bool PackedPair = false>
  __attribute__((optimize("O3"))) static void render_du4_index_frames(
      uint8_t* frames, size_t frame_bytes, const uint16_t* indices,
      size_t total_groups, const uint16_t* active_groups, size_t active_count,
      const uint8_t* const* luts, du4_render_timing_t& timing)
  {
    static_assert(!PackedPair || Count == 2, "Packed pair has two outputs");
    const int64_t begin = esp_timer_get_time();
    if (active_groups)
      for (unsigned frame = 0; frame < Count; ++frame)
        clear_du4_frame(frames + frame * frame_bytes, frame_bytes);
    const int64_t pixels_begin = esp_timer_get_time();
    timing.clear_us += pixels_begin - begin;
    uint32_t* outputs[Count];
    for (unsigned frame = 0; frame < Count; ++frame)
      outputs[frame] = reinterpret_cast<uint32_t*>(frames + frame * frame_bytes);
    const size_t count = active_groups ? active_count : total_groups;
    for (size_t active = 0; active < count; ++active) {
      const size_t group = active_groups ? active_groups[active] : active;
      uint32_t words[Count];
      if constexpr (PackedPair) {
        const auto pair = lgfx_epd_gc16_dual::group<MYPAPER_EPD_OUTPUT_PERMUTE == 1,true>(
            indices + (group << 3), nullptr, nullptr, luts[0]);
        words[0] = pair.first;
        words[1] = pair.second;
      } else {
        lgfx_epd_du4::group_indexed<Count,MYPAPER_EPD_OUTPUT_PERMUTE == 1>(
            indices + (group << 3), luts, words);
      }
      for (unsigned frame = 0; frame < Count; ++frame)
        outputs[frame][group] = words[frame];
#if MYPAPER_EPD_2D_DU4_BUS_RELIEF_GROUPS > 0
      // Preserve the existing USB-audio bus relief per output-frame volume.
      if ((active + 1) % std::max<unsigned>(1, MYPAPER_EPD_2D_DU4_BUS_RELIEF_GROUPS / Count) == 0)
        du4_bus_relief();
#endif
    }
    timing.pixels_us += esp_timer_get_time() - pixels_begin;
  }

  // DU4 has only four target buckets.  Collapse the two target nibbles to a
  // four-bit code and use a 4096-entry pair table instead of splitting and
  // looking up both pixels for every phase frame.  Exact unchanged 16-pixel
  // groups take an all-idle shortcut on phases whose diagonal is idle.
  __attribute__((optimize("O3"))) static void render_du4_phase_frame(
      uint8_t* output_frame, size_t output_bytes,
      const uint8_t* physical, const uint8_t* target,
      size_t panel_width, size_t panel_height,
      size_t memory_width, size_t scan_rows,
      uint16_t update_x, uint16_t update_y,
      uint16_t update_width, uint16_t update_height,
      const uint8_t* pair_lut, const uint8_t* target_code_lut,
      bool equal_pairs_idle,
      const uint16_t* active_groups, size_t active_group_count,
      du4_render_timing_t* timing = nullptr)
  {
    const auto& pair_shifts = lgfx_epd_pair_indices::output_shifts<MYPAPER_EPD_OUTPUT_PERMUTE == 1>;
    const size_t stride = panel_width >> 1;
    const size_t groups_per_row = memory_width >> 4;
    const size_t output_stride = memory_width >> 2;
    const bool full_screen = update_x == 0 && update_y == 0
                          && update_width == panel_width
                          && update_height == panel_height
                          && scan_rows == panel_height;

    if (full_screen && active_groups != nullptr) {
      const int64_t clear_begin = timing ? esp_timer_get_time() : 0;
      clear_du4_frame(output_frame, output_bytes);
      const int64_t pixels_begin = timing ? esp_timer_get_time() : 0;
      if (timing) timing->clear_us += pixels_begin - clear_begin;
      uint32_t* output = reinterpret_cast<uint32_t*>(output_frame);
      for (size_t active = 0; active < active_group_count; ++active) {
        const size_t group_index = active_groups[active];
        const size_t base = group_index << 3;
        uint32_t packed = 0;
        for (unsigned pair = 0; pair < 8; ++pair) {
          const uint8_t from = physical[base + pair];
          const uint8_t target_code = target_code_lut[target[base + pair]];
          packed |= uint32_t(pair_lut[(unsigned(from) << 4) | target_code])
                 << pair_shifts[pair];
        }
        output[group_index] = packed;
#if MYPAPER_EPD_2D_DU4_BUS_RELIEF_GROUPS > 0
        if ((active + 1) % MYPAPER_EPD_2D_DU4_BUS_RELIEF_GROUPS == 0) {
          du4_bus_relief();
        }
#endif
      }
      if (timing) timing->pixels_us += esp_timer_get_time() - pixels_begin;
      return;
    }

    if (!full_screen) {
      const int64_t clear_begin = timing ? esp_timer_get_time() : 0;
      clear_du4_frame(output_frame, output_bytes);
      if (timing) timing->clear_us += esp_timer_get_time() - clear_begin;
    }
    const int64_t pixels_begin = timing ? esp_timer_get_time() : 0;
    const size_t byte_begin = full_screen ? 0
        : std::min<size_t>(update_x >> 1, stride);
    const size_t byte_end = full_screen ? stride : std::min<size_t>(
        (size_t(update_x) + update_width) >> 1, stride);
    const size_t row_begin = full_screen ? 0 : std::min<size_t>(
        update_y, std::min(panel_height, scan_rows));
    const size_t row_end = full_screen ? scan_rows : std::min<size_t>(
        size_t(update_y) + update_height, std::min(panel_height, scan_rows));
    const size_t group_begin = byte_begin >> 3;
    const size_t group_end = std::min<size_t>(
        (byte_end + 7) >> 3, groups_per_row);

    for (size_t y = row_begin; y < row_end; ++y) {
      uint32_t* output = reinterpret_cast<uint32_t*>(
          output_frame + y * output_stride);
      const uint8_t* from_row = physical + y * stride;
      const uint8_t* to_row = target + y * stride;
      for (size_t group = group_begin; group < group_end; ++group) {
        const size_t base = group << 3;
        const bool complete_group = base >= byte_begin
                                 && base + 8 <= byte_end
                                 && base + 8 <= stride;
        if (equal_pairs_idle && complete_group) {
          uint64_t from_group;
          uint64_t to_group;
          memcpy(&from_group, from_row + base, sizeof(from_group));
          memcpy(&to_group, to_row + base, sizeof(to_group));
          if (from_group == to_group) {
            output[group] = 0;
            continue;
          }
        }

        uint32_t packed = 0;
        for (unsigned pair = 0; pair < 8; ++pair) {
          const size_t index = base + pair;
          if (complete_group
           || (index >= byte_begin && index < byte_end && index < stride)) {
            const uint8_t from = from_row[index];
            const uint8_t target_code = target_code_lut[to_row[index]];
            packed |= uint32_t(pair_lut[(unsigned(from) << 4) | target_code])
                   << pair_shifts[pair];
          }
        }
        output[group] = packed;
      }
#if MYPAPER_EPD_2D_DU4_BUS_RELIEF_ROWS > 0
      if ((y - row_begin + 1) % MYPAPER_EPD_2D_DU4_BUS_RELIEF_ROWS == 0) {
        du4_bus_relief();
      }
#endif
    }
    if (timing) timing->pixels_us += esp_timer_get_time() - pixels_begin;
  }

  __attribute__((optimize("O3"))) static void render_du4_three_frames(
      uint8_t* frames, size_t frame_bytes, const uint8_t* physical,
      const uint8_t* target, size_t total_groups, const uint16_t* active_groups,
      size_t active_count, const uint8_t* codes, const uint8_t* lut0,
      const uint8_t* lut1, const uint8_t* lut2, du4_render_timing_t& timing)
  {
    const int64_t clear_begin = esp_timer_get_time();
    if (active_groups) {
      for (unsigned frame = 0; frame < 3; ++frame)
        clear_du4_frame(frames + frame * frame_bytes, frame_bytes);
    }
    const int64_t pixels_begin = esp_timer_get_time();
    timing.clear_us += pixels_begin - clear_begin;
    auto* out0 = reinterpret_cast<uint32_t*>(frames);
    auto* out1 = reinterpret_cast<uint32_t*>(frames + frame_bytes);
    auto* out2 = reinterpret_cast<uint32_t*>(frames + frame_bytes * 2);
    const size_t count = active_groups ? active_count : total_groups;
    for (size_t active = 0; active < count; ++active) {
      const size_t group = active_groups ? active_groups[active] : active;
      uint32_t word0, word1, word2;
      lgfx_epd_du4::group3<MYPAPER_EPD_OUTPUT_PERMUTE == 1>(physical + (group << 3), target + (group << 3),
                         codes, lut0, lut1, lut2, word0, word1, word2);
      out0[group] = word0;
      out1[group] = word1;
      out2[group] = word2;
#if MYPAPER_EPD_2D_DU4_BUS_RELIEF_GROUPS > 0
      // Keep external-memory burst relief proportional to three outputs.
      if ((active + 1) % std::max<unsigned>(1, MYPAPER_EPD_2D_DU4_BUS_RELIEF_GROUPS / 3) == 0)
        du4_bus_relief();
#endif
    }
    timing.pixels_us += esp_timer_get_time() - pixels_begin;
  }

  static constexpr int DU4_PIPELINE_MAX_PHASES = 64;

  struct du4_phase_pipeline_t
  {
    const uint16_t* indices = nullptr;
    drive_frame_timing_t* timings = nullptr;
    uint8_t* frames = nullptr;
    size_t frame_bytes = 0;
    const uint8_t* physical = nullptr;
    const uint8_t* target = nullptr;
    uint8_t* pair_lut = nullptr;
    const uint8_t* unique_transitions = nullptr;
    size_t panel_width = 0;
    size_t panel_height = 0;
    size_t memory_width = 0;
    size_t scan_rows = 0;
    uint16_t update_x = 0;
    uint16_t update_y = 0;
    uint16_t update_width = 0;
    uint16_t update_height = 0;
    const uint16_t* active_groups = nullptr;
    size_t active_group_count = 0;
    int unique_count = 0;
    volatile int next_unique = 0;
    volatile bool ready[DU4_PIPELINE_MAX_PHASES] = {};
    volatile bool abort = false;
    volatile bool finished = false;
    volatile bool failed = false;
    bool external_stack = false;
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t done = nullptr;
    uint64_t frame_render_us = 0;
  };

  static void render_du4_pipeline_unique(du4_phase_pipeline_t* ctx,
                                         int unique)
  {
    const uint8_t* transition =
        ctx->unique_transitions + size_t(unique) * 256;
    const int64_t render_begin = esp_timer_get_time();
    build_du4_pair_lut(ctx->pair_lut, transition);
    const int64_t lut_end = esp_timer_get_time();
    du4_render_timing_t detail;
    if (ctx->indices) {
      const uint8_t* luts[] = {ctx->pair_lut};
      render_du4_index_frames<1>(ctx->frames + size_t(unique) * ctx->frame_bytes,
          ctx->frame_bytes, ctx->indices, (ctx->memory_width >> 4) * ctx->scan_rows,
          ctx->active_groups, ctx->active_group_count, luts, detail);
    } else {
    render_du4_phase_frame(
        ctx->frames + size_t(unique) * ctx->frame_bytes,
        ctx->frame_bytes, ctx->physical, ctx->target,
        ctx->panel_width, ctx->panel_height,
        ctx->memory_width, ctx->scan_rows,
        ctx->update_x, ctx->update_y,
        ctx->update_width, ctx->update_height,
        ctx->pair_lut, ctx->pair_lut + DU4_PAIR_LUT_BYTES,
        du4_equal_pairs_are_idle(transition),
        ctx->active_groups, ctx->active_group_count, &detail);
    }
    ctx->frame_render_us += esp_timer_get_time() - render_begin;
    if (ctx->timings) {
      auto& timing = ctx->timings[unique];
      timing.lut_us = lut_end - render_begin;
      timing.clear_us = detail.clear_us;
      timing.pixels_us = detail.pixels_us;
      timing.total_us = esp_timer_get_time() - render_begin;
    }
    __atomic_store_n(&ctx->ready[unique], true, __ATOMIC_RELEASE);
  }

  static void du4_phase_producer_task(void* argument)
  {
    auto* ctx = static_cast<du4_phase_pipeline_t*>(argument);
    for (int unique = ctx->next_unique;
         unique < ctx->unique_count && !ctx->abort;) {
      int count = 1;
#if MYPAPER_EPD_2D_DU4_DUAL_WORKER
      if (ctx->indices && unique + 1 < ctx->unique_count) {
        const int64_t begin = esp_timer_get_time();
        lgfx_epd_du4::build_dual_lut(ctx->pair_lut,
            ctx->unique_transitions + size_t(unique) * 256,
            ctx->unique_transitions + size_t(unique + 1) * 256);
        const int64_t lut_end = esp_timer_get_time();
        const uint8_t* luts[] = {ctx->pair_lut};
        du4_render_timing_t detail;
        render_du4_index_frames<2,true>(ctx->frames + size_t(unique) * ctx->frame_bytes,
            ctx->frame_bytes, ctx->indices, (ctx->memory_width >> 4) * ctx->scan_rows,
            ctx->active_groups, ctx->active_group_count, luts, detail);
        const int64_t end = esp_timer_get_time();
        ctx->frame_render_us += end - begin;
        if (ctx->timings) {
          auto& timing = ctx->timings[unique];
          timing.lut_us = lut_end - begin;
          timing.clear_us = detail.clear_us;
          timing.pixels_us = detail.pixels_us;
          timing.total_us = end - begin;
          timing.count = 2;
        }
        // These are dedicated slots (no DU4 ring reuse). Publish only after
        // both outputs are complete; animation demand generation stays single.
        __atomic_store_n(&ctx->ready[unique], true, __ATOMIC_RELEASE);
        __atomic_store_n(&ctx->ready[unique + 1], true, __ATOMIC_RELEASE);
        count = 2;
      }
#endif
      if (count == 1) render_du4_pipeline_unique(ctx, unique);
      unique += count;
      __atomic_store_n(&ctx->next_unique, unique, __ATOMIC_RELEASE);
    }
    const bool external_stack = ctx->external_stack;
    ctx->finished = true;
    xSemaphoreGive(ctx->done);
    if (external_stack) {
      vTaskDeleteWithCaps(nullptr);
    } else {
      vTaskDelete(nullptr);
    }
  }

  static void finish_du4_phase_pipeline(du4_phase_pipeline_t* ctx,
                                        bool abort)
  {
    if (ctx->done == nullptr) {
      return;
    }
    if (abort) {
      ctx->abort = true;
    }
    xSemaphoreTake(ctx->done, portMAX_DELAY);
    vSemaphoreDelete(ctx->done);
    ctx->done = nullptr;
    ctx->task = nullptr;
  }

  struct animation_pipeline_t {
    du4_phase_pipeline_t* source = nullptr;
    uint8_t* frames = nullptr;
    const uint16_t* map = nullptr;
    const lgfx_epd_page_turn::Layout* layout = nullptr;
    int waveform_phases = 15;
    int count = 24;
    int spread = lgfx_epd_page_turn::fast_spread;
    int slots = 24;
    int next = 0;
    int ready[38] = {}; // Up to 18 base phases + slow spread 20; -1 means free.
    bool abort = false;
    bool finished = false;
    bool external_stack = false;
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t done = nullptr;
    uint64_t zero_us = 0, copy_us = 0;
  };

  static void render_animation_phase(animation_pipeline_t* ctx,int phase) {
    lgfx_epd_page_turn::for_each_dependency(phase,ctx->waveform_phases,ctx->map,[&](int unique) {
      if (!ctx->source->ready[unique]) {
        render_du4_pipeline_unique(ctx->source,unique);
        ctx->source->next_unique=ctx->source->next_unique+1;
      }
    },ctx->spread,ctx->layout->band_count);
    const int slot=phase%ctx->slots;
    auto *out=ctx->frames+size_t(slot)*ctx->source->frame_bytes;
    const int64_t begin=esp_timer_get_time();
    lgfx_epd_page_turn::clear_inactive_layout(out,ctx->source->frame_bytes,
        ctx->waveform_phases,phase,*ctx->layout,ctx->spread);
    const int64_t copied=esp_timer_get_time();
    lgfx_epd_page_turn::copy_layout(out,ctx->source->frame_bytes,ctx->source->frames,
        ctx->map,ctx->waveform_phases,phase,*ctx->layout,false,ctx->spread);
    ctx->zero_us+=copied-begin;
    const int64_t end=esp_timer_get_time();
    ctx->copy_us+=end-copied;
    __atomic_store_n(&ctx->ready[slot],phase,__ATOMIC_RELEASE);
    ctx->next=phase+1;
  }

  static void animation_producer_task(void* argument) {
    auto *ctx=static_cast<animation_pipeline_t*>(argument);
    for (int phase=ctx->next;phase<ctx->count &&
         !__atomic_load_n(&ctx->abort,__ATOMIC_ACQUIRE);++phase) {
      // A slot remains owned through the following successful scan. That
      // scan's submission stops/resets the previous DMA descriptor chain.
      while (__atomic_load_n(&ctx->ready[phase%ctx->slots],__ATOMIC_ACQUIRE)!=-1 &&
             !__atomic_load_n(&ctx->abort,__ATOMIC_ACQUIRE)) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
      }
      if (__atomic_load_n(&ctx->abort,__ATOMIC_ACQUIRE)) break;
      render_animation_phase(ctx,phase);
    }
    const bool external=ctx->external_stack;
    __atomic_store_n(&ctx->finished,true,__ATOMIC_RELEASE);
    xSemaphoreGive(ctx->done);
    if (external) vTaskDeleteWithCaps(nullptr);
    else vTaskDelete(nullptr);
  }

  static void finish_animation_pipeline(animation_pipeline_t* ctx,bool abort) {
    if (!ctx->done) return;
    if (abort) {
      __atomic_store_n(&ctx->abort,true,__ATOMIC_RELEASE);
      if (!__atomic_load_n(&ctx->finished,__ATOMIC_ACQUIRE) && ctx->task)
        xTaskNotifyGive(ctx->task);
    }
    xSemaphoreTake(ctx->done,portMAX_DELAY);
    vSemaphoreDelete(ctx->done);
    ctx->done=nullptr;
    ctx->task=nullptr;
  }

  struct phase_line_context_t {
    const uint8_t* physical;
    const uint8_t* target;
    const uint8_t* transition;
    size_t panel_width;
    size_t panel_height;
    size_t memory_width;
    uint16_t update_x;
    uint16_t update_y;
    uint16_t update_width;
    uint16_t update_height;
  };

  __attribute__((optimize("O3"))) static void render_phase_line(
      void* argument, size_t y, uint8_t* output_bytes, size_t output_len)
  {
    auto* ctx = static_cast<phase_line_context_t*>(argument);
    const size_t expected_len = ctx->memory_width >> 2;
    if (output_len < expected_len) {
      memset(output_bytes, 0, output_len);
      return;
    }
    auto* output = reinterpret_cast<uint32_t*>(output_bytes);
    const size_t groups_per_row = ctx->memory_width >> 4;
    const size_t stride = ctx->panel_width >> 1;
    const size_t row_end = size_t(ctx->update_y) + ctx->update_height;
    if (y >= ctx->panel_height || y < ctx->update_y || y >= row_end) {
      memset(output_bytes, 0, expected_len);
      return;
    }

    static constexpr uint8_t pair_shifts[8] = {
      4, 0, 12, 8, 20, 16, 28, 24
    };
    const size_t byte_begin = ctx->update_x >> 1;
    const size_t byte_end = (size_t(ctx->update_x) + ctx->update_width) >> 1;
    const bool full_row = byte_begin == 0 && byte_end == stride;
    const uint8_t* from_row = ctx->physical + y * stride;
    const uint8_t* to_row = ctx->target + y * stride;
    for (size_t group = 0; group < groups_per_row; ++group) {
      uint32_t packed = 0;
      const size_t base = group << 3;
      if (full_row) {
        for (unsigned pair = 0; pair < 8; ++pair) {
          packed |= uint32_t(drive_pixel_pair(
              from_row[base + pair], to_row[base + pair], ctx->transition))
                    << pair_shifts[pair];
        }
      } else {
        for (unsigned pair = 0; pair < 8; ++pair) {
          const size_t index = base + pair;
          if (index >= byte_begin && index < byte_end && index < stride) {
            packed |= uint32_t(drive_pixel_pair(
                from_row[index], to_row[index], ctx->transition))
                      << pair_shifts[pair];
          }
        }
      }
      output[group] = permute_epd_output(packed);
    }
  }

  // Commit only transitions for which the selected waveform actually emitted
  // a drive pulse.  The supplied DU table drives target levels 0 and 15 only;
  // levels 1..14 are all-zero rows and therefore leave the particles at their
  // previous physical state.  Blindly copying the logical target after DU
  // makes the next differential GC16 lookup use a false old-gray value.
  __attribute__((optimize("O3"))) static size_t commit_physical_rect_4bpp(
      uint8_t* physical, const uint8_t* target, size_t stride,
      uint16_t x, uint16_t y, uint16_t width, uint16_t height,
      const EpdWaveformPhases* phases, uint8_t waveform_type)
  {
    bool transition_driven[256] = {};
    for (int phase = 0; phase < phases->phases; ++phase) {
      const uint8_t* phase_lut = phases->luts + size_t(phase) * 64;
      for (unsigned from = 0; from < 16; ++from) {
        for (unsigned to = 0; to < 16; ++to) {
          transition_driven[(from << 4) | to] |=
              waveform_drive(phase_lut, from, to) != 0;
        }
      }
    }

    const size_t begin = x >> 1;
    const size_t bytes = width >> 1;
    size_t retained = 0;
    for (size_t row = y; row < size_t(y) + height; ++row) {
      uint8_t* physical_row = physical + row * stride + begin;
      const uint8_t* target_row = target + row * stride + begin;
      for (size_t i = 0; i < bytes; ++i) {
        const uint8_t old_pair = physical_row[i];
        const uint8_t new_pair = target_row[i];
        const uint8_t old_high = old_pair >> 4;
        const uint8_t old_low = old_pair & 0x0F;
        const uint8_t new_high = new_pair >> 4;
        const uint8_t new_low = new_pair & 0x0F;
        const bool high_driven = transition_driven[(old_high << 4) | new_high];
        const bool low_driven = transition_driven[(old_low << 4) | new_low];
        const uint8_t physical_high = waveform_type == WAVEFORM_MODE_DU4
                                    ? quantize_du4_gray(new_high)
                                    : (waveform_type == WAVEFORM_MODE_A2
                                    || waveform_type == WAVEFORM_MODE_DU
                                      ? quantize_a2_gray(new_high) : new_high);
        const uint8_t physical_low = waveform_type == WAVEFORM_MODE_DU4
                                   ? quantize_du4_gray(new_low)
                                   : (waveform_type == WAVEFORM_MODE_A2
                                   || waveform_type == WAVEFORM_MODE_DU
                                     ? quantize_a2_gray(new_low) : new_low);
        const uint8_t resolved_high = high_driven ? physical_high : old_high;
        const uint8_t resolved_low = low_driven ? physical_low : old_low;
        physical_row[i] = (resolved_high << 4) | resolved_low;
        retained += (!high_driven && old_high != physical_high)
                  + (!low_driven && old_low != physical_low);
      }
    }
    return retained;
  }

  // Refresh owns the physical state for the whole batch. Idle preparation
  // only tries the same mutex; no concurrent scan/cache read or worker task.
  struct StatsLock {
    SemaphoreHandle_t semaphore;
    ~StatsLock() { xSemaphoreGive(semaphore); }
  };

  Panel_EPD_2D::Panel_EPD_2D(void)
  : Panel_EPD()
  {
  }

  Panel_EPD_2D::~Panel_EPD_2D(void)
  {
    // The autodetected panel lives for the process lifetime.  Keep teardown
    // deliberately passive so a late static destructor cannot race the EPD
    // task or power rails during system shutdown/restart.
  }

  bool Panel_EPD_2D::init(bool use_reset)
  {
    ESP_LOGI(TAG_2D, "init begin: panel=%ux%u memory=%ux%u",
             (unsigned)_cfg.panel_width, (unsigned)_cfg.panel_height,
             (unsigned)_cfg.memory_width, (unsigned)_cfg.memory_height);

    if (_waveform == nullptr) {
      _waveform = lgfx_epd_2d_waveform_provider();
    }
    if (_waveform == nullptr) {
      ESP_LOGE(TAG_2D, "no panel waveform supplied");
      return false;
    }

    if (!Panel_HasBuffer::init(use_reset)) {
      ESP_LOGE(TAG_2D, "base panel init failed");
      return false;
    }
    if (!init_internal()) {
      ESP_LOGE(TAG_2D, "internal init failed");
      return false;
    }

    const size_t frame_bytes = (size_t(_cfg.panel_width) * _cfg.panel_height) >> 1;
    memset(_buf, 0xFF, frame_bytes);
    memset(_physical_buf, 0xFF, frame_bytes);
    memset(_target_buf, 0xFF, frame_bytes);
    _range_mod.top = INT16_MAX;
    _range_mod.left = INT16_MAX;
    _range_mod.right = 0;
    _range_mod.bottom = 0;

    ESP_LOGI(TAG_2D,
             "init complete: waveform_modes=%u temp_ranges=%u initial_state=assume-white",
             (unsigned)_waveform->num_modes,
             (unsigned)_waveform->num_temp_ranges);
    return true;
  }

  bool Panel_EPD_2D::init_internal(void)
  {
    const size_t memory_w = _cfg.memory_width;
    const size_t memory_h = _cfg.memory_height;
    const size_t panel_w = _cfg.panel_width;
    const size_t panel_h = _cfg.panel_height;
    if (memory_w == 0 || memory_h == 0 || panel_w == 0 || panel_h == 0
     || (memory_w & 15) != 0 || (panel_w & 1) != 0) {
      ESP_LOGE(TAG_2D, "unsupported dimensions: panel=%ux%u memory=%ux%u",
               (unsigned)panel_w, (unsigned)panel_h,
               (unsigned)memory_w, (unsigned)memory_h);
      return false;
    }

    const size_t frame_bytes = (panel_w * panel_h) >> 1;
    const size_t dma_len = (memory_w >> 2) + config_detail().line_padding;
    _buf = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(16, frame_bytes, MALLOC_CAP_SPIRAM));
    _physical_buf = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(16, frame_bytes, MALLOC_CAP_SPIRAM));
    _target_buf = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(16, frame_bytes, MALLOC_CAP_SPIRAM));
#if (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31)) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
    _scan_frame_bytes_2d = (memory_w >> 2) * memory_h;
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    // Reserve before UI font/book allocations can fragment PSRAM. Text and
    // GC16/HD batches are serialized by this panel's single refresh task, so
    // they can borrow the same storage rather than retain two large caches.
    const size_t arena_bytes = size_t(MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS)
                             * _scan_frame_bytes_2d;
    if (!_phase_arena_2d) {
      _phase_arena_2d = static_cast<uint8_t*>(heap_caps_aligned_alloc(
          64, arena_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
      if (!_phase_arena_2d) {
        ESP_LOGE(TAG_2D, "S31 phase arena allocation failed: bytes=%u free=%u largest=%u",
                 (unsigned)arena_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return false;
      }
      _phase_arena_capacity_2d = arena_bytes;
      ESP_LOGI(TAG_2D, "S31 shared phase arena ready: slots=%d bytes=%u",
               MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS, (unsigned)arena_bytes);
    }
#endif
    _scan_frame_2d = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, _scan_frame_bytes_2d,
        MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
    _du4_pair_lut_2d = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        16, DU4_RENDER_LUT_BYTES,
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
#if MYPAPER_EPD_2D_GC16_PAIR_LUT
    _gc16_pair_lut_2d = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, GC16_PAIR_LUT_BYTES,
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
#endif
    const size_t du4_group_capacity = (memory_w >> 4) * memory_h;
    if (du4_group_capacity <= UINT16_MAX) {
      _du4_active_groups_2d = static_cast<uint16_t*>(heap_caps_malloc(
          du4_group_capacity * sizeof(uint16_t),
          MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
      if (_du4_active_groups_2d != nullptr) {
        _du4_active_group_capacity_2d = du4_group_capacity;
      }
    }
#else
    _dma_bufs_2d[0] = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(4, dma_len, MALLOC_CAP_DMA));
    _dma_bufs_2d[1] = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(4, dma_len, MALLOC_CAP_DMA));
#endif

    if (_buf == nullptr || _physical_buf == nullptr || _target_buf == nullptr
#if (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31)) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
     || _scan_frame_2d == nullptr
#else
     || _dma_bufs_2d[0] == nullptr || _dma_bufs_2d[1] == nullptr
#endif
       ) {
      ESP_LOGE(TAG_2D,
               "allocation failed: draw=%p physical=%p target=%p dma=%p/%p scan=%p",
               _buf, _physical_buf, _target_buf,
               _dma_bufs_2d[0], _dma_bufs_2d[1], _scan_frame_2d);
      return false;
    }
#if (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31)) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
    memset(_scan_frame_2d, 0, _scan_frame_bytes_2d);
    if (_du4_pair_lut_2d == nullptr) {
      ESP_LOGW(TAG_2D,
               "DU4 pair LUT allocation failed; generic renderer will be used");
    } else {
      build_du4_target_code_lut(
          _du4_pair_lut_2d + DU4_PAIR_LUT_BYTES);
    }
#if MYPAPER_EPD_2D_GC16_PAIR_LUT
    if (_gc16_pair_lut_2d == nullptr) {
      ESP_LOGW(TAG_2D,
               "GC16 pair LUT allocation failed; generic renderer will be used");
    } else {
      ESP_LOGI(TAG_2D,
               "GC16 pair LUT ready: bytes=%u memory=internal",
               (unsigned)GC16_PAIR_LUT_BYTES);
    }
#endif
    if (_du4_active_groups_2d == nullptr) {
      ESP_LOGW(TAG_2D,
               "DU4 active group allocation failed; full-frame renderer will be used");
    }
#else
    memset(_dma_bufs_2d[0], 0, dma_len);
    memset(_dma_bufs_2d[1], 0, dma_len);
#endif

    if (selectWaveform(epd_mode_t::epd_quality).phases == nullptr) {
      ESP_LOGE(TAG_2D, "waveform has no GC16/type-2 Quality mode");
      return false;
    }

    _stats_mutex = xSemaphoreCreateMutex();
    if (!_stats_mutex) return false;
    _update_queue_2d = xQueueCreate(8, sizeof(update_data_t));
    if (_update_queue_2d == nullptr) {
      ESP_LOGE(TAG_2D, "queue creation failed");
      return false;
    }

    uint8_t priority = config_detail().task_priority;
    // MyPaper reserves core 1 for the EPD update path.  Do not derive this
    // from the caller's current core: doing so makes ownership depend on the
    // initialization path and allows network/application work to contend with
    // phase generation and scan submission.
    uint8_t core = portNUM_PROCESSORS > 1 ? 1 : 0;
    const BaseType_t result = xTaskCreatePinnedToCore(
        reinterpret_cast<TaskFunction_t>(task_update), "epd2d", 6144, this,
        priority, &_task_update_2d, core);
    if (result != pdPASS) {
      ESP_LOGE(TAG_2D, "task creation failed: %ld", (long)result);
      return false;
    }

    ESP_LOGI(TAG_2D,
             "buffers ready: draw=%u physical=%u target=%u dma_line=%u scan_frame=%u task=%u/core%u",
             (unsigned)frame_bytes, (unsigned)frame_bytes,
             (unsigned)frame_bytes, (unsigned)dma_len,
             (unsigned)_scan_frame_bytes_2d,
             (unsigned)priority, (unsigned)core);
    return true;
  }

  Panel_EPD_2D::selected_waveform_t
  Panel_EPD_2D::selectWaveform(epd_mode_t mode) const
  {
    selected_waveform_t selected;
    if (_waveform == nullptr || _waveform->mode_data == nullptr) {
      return selected;
    }

    uint8_t requested_type = WAVEFORM_MODE_DU;
    if (mode == epd_mode_t::epd_quality) {
      requested_type = WAVEFORM_MODE_GC16;
    } else if (mode == epd_mode_t::epd_hd) {
      requested_type = WAVEFORM_MODE_HD;
    } else if (mode == epd_mode_t::epd_text) {
      requested_type = WAVEFORM_MODE_DU4;
    } else if (mode == epd_mode_t::epd_fastest) {
      requested_type = WAVEFORM_MODE_A2;
    }

    const EpdWaveformMode* waveform_mode = nullptr;
    for (unsigned i = 0; i < _waveform->num_modes; ++i) {
      const EpdWaveformMode* candidate = _waveform->mode_data[i];
      if (candidate != nullptr && candidate->type == requested_type) {
        waveform_mode = candidate;
        break;
      }
    }
    // DU4 is optional. Prefer GL16, then GC16, as safe text fallbacks.
    if (waveform_mode == nullptr && requested_type == WAVEFORM_MODE_DU4) {
      for (unsigned i = 0; i < _waveform->num_modes; ++i) {
        const EpdWaveformMode* candidate = _waveform->mode_data[i];
        if (candidate != nullptr && candidate->type == WAVEFORM_MODE_GL16) {
          waveform_mode = candidate;
          requested_type = WAVEFORM_MODE_GL16;
          break;
        }
      }
    }
    if (waveform_mode == nullptr
     && (requested_type == WAVEFORM_MODE_DU4
      || requested_type == WAVEFORM_MODE_GL16)) {
      for (unsigned i = 0; i < _waveform->num_modes; ++i) {
        const EpdWaveformMode* candidate = _waveform->mode_data[i];
        if (candidate != nullptr && candidate->type == WAVEFORM_MODE_GC16) {
          waveform_mode = candidate;
          requested_type = WAVEFORM_MODE_GC16;
          break;
        }
      }
    }
    // A2 is optional; retain the validated DU path as its fallback.
    if (waveform_mode == nullptr && requested_type == WAVEFORM_MODE_A2) {
      for (unsigned i = 0; i < _waveform->num_modes; ++i) {
        const EpdWaveformMode* candidate = _waveform->mode_data[i];
        if (candidate != nullptr && candidate->type == WAVEFORM_MODE_DU) {
          waveform_mode = candidate;
          requested_type = WAVEFORM_MODE_DU;
          break;
        }
      }
    }
    if (waveform_mode == nullptr || waveform_mode->range_data == nullptr
     || waveform_mode->temp_ranges == 0) {
      return selected;
    }

    unsigned range = 0;
    if (_waveform->temp_intervals != nullptr && _waveform->num_temp_ranges != 0) {
      unsigned closest = 0;
      int closest_distance = INT_MAX;
      const unsigned count = std::min<unsigned>(waveform_mode->temp_ranges,
                                                 _waveform->num_temp_ranges);
      for (unsigned i = 0; i < count; ++i) {
        const auto& interval = _waveform->temp_intervals[i];
        if (_temperature_c >= interval.min && _temperature_c < interval.max) {
          range = i;
          closest_distance = 0;
          break;
        }
        const int distance = (_temperature_c < interval.min)
                           ? interval.min - _temperature_c
                           : _temperature_c - interval.max;
        if (distance < closest_distance) {
          closest = i;
          closest_distance = distance;
        }
      }
      if (closest_distance != 0) {
        range = closest;
      }
    }

    selected.phases = waveform_mode->range_data[range];
    selected.type = requested_type;
    selected.temperature_range = range;
    return selected;
  }

  void Panel_EPD_2D::waitDisplay(void)
  {
    while (displayBusy()) {
      vTaskDelay(1);
    }
  }

  bool Panel_EPD_2D::displayBusy(void)
  {
    return _display_busy_2d
        || (_update_queue_2d != nullptr
         && uxQueueMessagesWaiting(_update_queue_2d) != 0);
  }

  bool Panel_EPD_2D::measureVCOM(uint32_t* vcom_mV, uint32_t timeout_ms)
  {
    waitDisplay();
    return getBusEPD2D()->measureVCOM(vcom_mV, timeout_ms);
  }

  bool Panel_EPD_2D::preparePhysicalGrayscale4Stats(
      const uint8_t* frame, uint16_t width, uint16_t height,
      uint8_t rotation, epd_mode_t mode)
  {
    if (!frame || !_stats_mutex || displayBusy() || !_physical_state_known
     || getDither() || width != _cfg.panel_width || height != _cfg.panel_height
     || width != _cfg.memory_width || height != _cfg.memory_height
     || width % 16 || _cfg.offset_x || _cfg.offset_y || rotation != getRotation()
     || xSemaphoreTake(_stats_mutex, 0) != pdTRUE) return false;
    StatsLock lock{_stats_mutex};
    if (displayBusy() || !_physical_state_known) return false;
    _prepared_stats.valid = false;
    const auto selected = selectWaveform(mode);
    if (!selected.phases || !selected.phases->luts) return false;
    const size_t bytes = size_t(width / 2) * height;
    const size_t groups = (size_t(width) >> 4) * height;
    if (!_prepared_stats.target)
      _prepared_stats.target = static_cast<uint8_t*>(heap_caps_malloc(bytes,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_prepared_stats.target) return false;
    const int64_t started = esp_timer_get_time();
    // Own a copy: reader buffers can be moved, freed or recycled before use.
    memcpy(_prepared_stats.target, frame, bytes);
    if (selected.type == WAVEFORM_MODE_DU || selected.type == WAVEFORM_MODE_A2)
      quantize_binary_target_rect_4bpp(_prepared_stats.target, width / 2,
                                     width, height, 0, 0, width, height);
    _prepared_stats.fused = false;
    if (selected.type == WAVEFORM_MODE_DU4 && groups <= UINT16_MAX) {
      if (!_prepared_stats.groups)
        _prepared_stats.groups = static_cast<uint16_t*>(heap_caps_malloc(
            groups * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (_prepared_stats.groups) {
        bool driven[256];
        build_driven_transition_mask(driven, selected.phases, selected.type);
        _prepared_stats.transition_count = lgfx_epd_stats::collect(
            _prepared_stats.used, _prepared_stats.groups, _prepared_stats.group_count,
            _physical_buf, _prepared_stats.target, bytes, driven);
        _prepared_stats.fused = true;
      }
    }
    if (!_prepared_stats.fused)
      _prepared_stats.transition_count = collect_used_transitions_4bpp(
          _prepared_stats.used, _physical_buf, _prepared_stats.target,
          width / 2, height, 0, 0, width, height);
    // The caller passes logical rotation for import validation; update_data_t
    // carries physical/internal rotation (including panel offset). Match keys
    // in that same coordinate system without changing animation rotation.
    _prepared_stats.key = {_physical_generation, _internal_rotation, selected.type, selected.phases};
    _prepared_stats.valid = true;
    ESP_LOGI(TAG_2D, "prefetch statistics ready: type=%u transitions=%u groups=%u time=%lld us",
             (unsigned)selected.type, (unsigned)_prepared_stats.transition_count,
             (unsigned)(_prepared_stats.fused ? _prepared_stats.group_count : 0),
             (long long)(esp_timer_get_time() - started));
    return true;
  }

  bool Panel_EPD_2D::copyPhysicalGrayscale4(uint8_t* destination, size_t capacity)
  {
    const size_t bytes = size_t(_cfg.memory_width) * _cfg.memory_height / 2;
    if (!destination || !_buf || capacity < bytes || displayBusy()) return false;
    memcpy(destination, _buf, bytes);
    return true;
  }

  bool Panel_EPD_2D::writePhysicalGrayscale4(const uint8_t* physical_frame,
                                            uint16_t physical_width,
                                            uint16_t physical_height,
                                            uint8_t rotation)
  {
    // Only the draw buffer is writable; target and physical state stay owned
    // by the refresh task. Reject queued/in-flight updates before any write.
    if (!physical_frame || !_buf || displayBusy()
     || physical_width != _cfg.panel_width || physical_height != _cfg.panel_height
     || _cfg.memory_width != _cfg.panel_width || (_cfg.panel_width & 1)
     || _cfg.offset_x != 0 || _cfg.offset_y != 0 || rotation != getRotation()) {
      return false;
    }
    const int64_t started = esp_timer_get_time();
    const size_t bytes = size_t(physical_width / 2) * physical_height;
    memcpy(_buf, physical_frame, bytes);
    _range_mod.left = 0;
    _range_mod.top = 0;
    _range_mod.right = physical_width - 1;
    _range_mod.bottom = physical_height - 1;
    ESP_LOGI(TAG_2D, "physical 4bpp import: bytes=%u rotation=%u copy=%lld us",
             (unsigned)bytes, (unsigned)rotation,
             (long long)(esp_timer_get_time() - started));
    return true;
  }

  bool Panel_EPD_2D::writeNativeGrayscale4(const uint8_t* logical_frame,
                                           uint16_t logical_width,
                                           uint16_t logical_height)
  {
    if (logical_frame == nullptr || _buf == nullptr) {
      return false;
    }
    if (logical_width != width() || logical_height != height()) {
      ESP_LOGW(TAG_2D,
               "native 4bpp import size mismatch: src=%ux%u logical=%ux%u panel=%ux%u",
               (unsigned)logical_width, (unsigned)logical_height,
               (unsigned)width(), (unsigned)height(),
               (unsigned)_cfg.panel_width, (unsigned)_cfg.panel_height);
      return false;
    }

    const int64_t t0 = esp_timer_get_time();
    const size_t src_stride = (size_t(logical_width) + 1) >> 1;
    const size_t dst_stride = size_t(_cfg.panel_width) >> 1;
    const size_t dst_bytes = dst_stride * _cfg.panel_height;
    const uint_fast8_t r = _internal_rotation;

    auto read_src = [&](uint16_t x, uint16_t y) -> uint8_t {
      const uint8_t packed = logical_frame[size_t(y) * src_stride + (x >> 1)];
      return (x & 1) ? (packed & 0x0F) : (packed >> 4);
    };
    auto write_dst = [&](uint16_t x, uint16_t y, uint8_t value) {
      uint8_t* dst = _buf + size_t(y) * dst_stride + (x >> 1);
      if (x & 1) {
        *dst = (*dst & 0xF0) | (value & 0x0F);
      } else {
        *dst = (*dst & 0x0F) | (value << 4);
      }
    };

    if (r == 0 && logical_width == _cfg.panel_width
     && logical_height == _cfg.panel_height && src_stride == dst_stride) {
      memcpy(_buf, logical_frame, dst_bytes);
    } else if (r == 3 && logical_width == _cfg.panel_height
            && logical_height == _cfg.panel_width
            && (logical_height & 1) == 0
            && (logical_width & 1) == 0) {
      for (uint16_t ly = 0; ly < logical_height; ly += 2) {
        const uint8_t* src0 = logical_frame + size_t(ly) * src_stride;
        const uint8_t* src1 = src0 + src_stride;
        uint8_t* dst_col = _buf + (ly >> 1);
        for (uint16_t sx_byte = 0; sx_byte < src_stride; ++sx_byte) {
          const uint8_t a = src0[sx_byte];
          const uint8_t b = src1[sx_byte];
          const uint16_t lx0 = sx_byte << 1;
          const uint16_t lx1 = lx0 + 1;
          dst_col[size_t(_cfg.panel_height - 1 - lx0) * dst_stride] = (a & 0xF0) | (b >> 4);
          dst_col[size_t(_cfg.panel_height - 1 - lx1) * dst_stride] = (uint8_t)((a << 4) | (b & 0x0F));
        }
      }
    } else {
      for (uint16_t ly = 0; ly < logical_height; ++ly) {
        for (uint16_t lx = 0; lx < logical_width; ++lx) {
          uint_fast16_t px = lx;
          uint_fast16_t py = ly;
          _rotate_pos(px, py);
          write_dst(px, py, read_src(lx, ly));
        }
      }
    }

    _range_mod.left = 0;
    _range_mod.top = 0;
    _range_mod.right = _cfg.panel_width - 1;
    _range_mod.bottom = _cfg.panel_height - 1;

    ESP_LOGI(TAG_2D,
             "native 4bpp import: src=%ux%u rotation=%u time=%lld ms",
             (unsigned)logical_width, (unsigned)logical_height,
             (unsigned)r,
             (esp_timer_get_time() - t0) / 1000);
    return true;
  }

  bool Panel_EPD_2D::writeNativeGrayscale4Rect(const uint8_t* logical_frame,
                                               uint16_t logical_width,
                                               uint16_t logical_height,
                                               int16_t logical_x,
                                               int16_t logical_y)
  {
    if (logical_frame == nullptr || _buf == nullptr ||
        logical_width == 0 || logical_height == 0 ||
        logical_x < 0 || logical_y < 0 ||
        logical_x + logical_width > width() ||
        logical_y + logical_height > height()) {
      return false;
    }

    const int64_t t0 = esp_timer_get_time();
    const size_t src_stride = (size_t(logical_width) + 1) >> 1;
    const size_t dst_stride = size_t(_cfg.panel_width) >> 1;
    const uint_fast8_t r = _internal_rotation;

    auto read_src = [&](uint16_t x, uint16_t y) -> uint8_t {
      const uint8_t packed = logical_frame[size_t(y) * src_stride + (x >> 1)];
      return (x & 1) ? (packed & 0x0F) : (packed >> 4);
    };

    // Reader images normally use rotation 3 and even dimensions. Copy two
    // logical scanlines into one physical packed-nibble column at a time.
    if (r == 3 && (logical_y & 1) == 0 && (logical_height & 1) == 0) {
      for (uint16_t sy = 0; sy < logical_height; sy += 2) {
        uint8_t* dst_col = _buf + ((logical_y + sy) >> 1);
        for (uint16_t sx = 0; sx < logical_width; ++sx) {
          const uint8_t high = read_src(sx, sy);
          const uint8_t low = read_src(sx, sy + 1);
          const size_t physical_y = _cfg.panel_height - 1 - (logical_x + sx);
          dst_col[physical_y * dst_stride] = uint8_t((high << 4) | low);
        }
      }
    } else {
      for (uint16_t sy = 0; sy < logical_height; ++sy) {
        for (uint16_t sx = 0; sx < logical_width; ++sx) {
          uint_fast16_t px = logical_x + sx;
          uint_fast16_t py = logical_y + sy;
          _rotate_pos(px, py);
          uint8_t* dst = _buf + size_t(py) * dst_stride + (px >> 1);
          const uint8_t value = read_src(sx, sy);
          if (px & 1) *dst = (*dst & 0xF0) | value;
          else        *dst = (*dst & 0x0F) | (value << 4);
        }
      }
    }

    // Native covers also serve partial shelf redraws. Mark only the imported
    // rectangle, just like the ordinary pixel writer (including rotation).
    uint_fast16_t xs=logical_x, ys=logical_y;
    uint_fast16_t xe=logical_x+logical_width-1, ye=logical_y+logical_height-1;
    _rotate_pos(xs,ys,xe,ye);
    _range_mod.left = std::min<int32_t>(_range_mod.left,xs);
    _range_mod.top = std::min<int32_t>(_range_mod.top,ys);
    _range_mod.right = std::max<int32_t>(_range_mod.right,xe);
    _range_mod.bottom = std::max<int32_t>(_range_mod.bottom,ye);
    ESP_LOGD(TAG_2D,
             "native 4bpp rect import: src=%ux%u dst=%d,%d rotation=%u time=%lld ms",
             (unsigned)logical_width, (unsigned)logical_height,
             (int)logical_x, (int)logical_y, (unsigned)r,
             (esp_timer_get_time() - t0) / 1000);
    return true;
  }

  bool Panel_EPD_2D::setPageTurnAnimation(int direction, int speed)
  {
    _next_animation_direction = 0;
    if (direction == 0) return true;
    if (displayBusy()) return false;
    if (!lgfx_epd_page_turn::valid_direction(direction)) return false;
    if (speed<1 || speed>2) return false;
    _next_animation_direction = direction;
    _next_animation_speed = uint8_t(speed);
    return true;
  }

  bool Panel_EPD_2D::setNextRefreshChangedPixelsOnly(bool enabled)
  {
    _next_changed_pixels_only = false;
    if (enabled && displayBusy()) return false;
    _next_changed_pixels_only = enabled;
    return true;
  }

  void Panel_EPD_2D::display(uint_fast16_t x, uint_fast16_t y,
                             uint_fast16_t w, uint_fast16_t h)
  {
    // Consume even on empty/invalid submissions; never leak into another UI.
    const bool changed_pixels_only = _next_changed_pixels_only;
    _next_changed_pixels_only = false;
    if (w != 0 && h != 0) {
      uint_fast16_t xs = x;
      uint_fast16_t ys = y;
      uint_fast16_t xe = x + w - 1;
      uint_fast16_t ye = y + h - 1;
      _rotate_pos(xs, ys, xe, ye);
      _range_mod.left = std::min<int32_t>(xs, _range_mod.left);
      _range_mod.right = std::max<int32_t>(xe, _range_mod.right);
      _range_mod.top = std::min<int32_t>(ys, _range_mod.top);
      _range_mod.bottom = std::max<int32_t>(ye, _range_mod.bottom);
    }
    if (_range_mod.empty()) {
      _next_animation_direction = 0;
      ESP_LOGD(TAG_2D, "display skipped: update range is empty");
      return;
    }

    _range_mod.left = std::max<int32_t>(0, _range_mod.left);
    _range_mod.right = std::min<int32_t>(_cfg.panel_width - 1, _range_mod.right);
    _range_mod.top = std::max<int32_t>(0, _range_mod.top);
    _range_mod.bottom = std::min<int32_t>(_cfg.panel_height - 1, _range_mod.bottom);
    if (_range_mod.empty()) {
      ESP_LOGE(TAG_2D, "display skipped: range outside panel");
      return;
    }

    const uint16_t xs = (_range_mod.left + _cfg.offset_x) & ~1u;
    const uint16_t xe = (_range_mod.right + _cfg.offset_x) | 1u;
    update_data_t update = {
      .x = xs,
      .y = uint16_t(_range_mod.top + _cfg.offset_y),
      .w = uint16_t(xe - xs + 1),
      .h = uint16_t(_range_mod.bottom - _range_mod.top + 1),
      .mode = _epd_mode,
      .animation_direction = _next_animation_direction,
      .animation_rotation = uint8_t(_internal_rotation),
      .animation_speed = _next_animation_speed,
      .changed_pixels_only = changed_pixels_only,
      .queued_at_us = esp_timer_get_time(),
    };
    _next_animation_direction = 0;

    // The sprite writeback deliberately does NOT happen here. It is a long,
    // non-interruptible walk over PSRAM, and this function runs on whichever
    // core the application task sits on - core 0, the same core as the USB host
    // ISR. Move it to task_update(), which is pinned to the scan core.
    _display_busy_2d = true;
    const bool queued = xQueueSend(_update_queue_2d, &update,
                                   128 / portTICK_PERIOD_MS) == pdTRUE;
    if (queued) {
      ESP_LOGD(TAG_2D, "refresh queued: x=%u y=%u w=%u h=%u mode=%u",
               update.x, update.y, update.w, update.h, (unsigned)update.mode);
      _range_mod.top = INT16_MAX;
      _range_mod.left = INT16_MAX;
      _range_mod.right = 0;
      _range_mod.bottom = 0;
    } else {
      _display_busy_2d = false;
      ESP_LOGE(TAG_2D, "refresh queue send failed");
    }
  }

  void Panel_EPD_2D::task_update(Panel_EPD_2D* me)
  {
    const size_t panel_w = me->_cfg.panel_width;
    const size_t panel_h = me->_cfg.panel_height;
    const size_t memory_w = me->_cfg.memory_width;
    const size_t stride = panel_w >> 1;
    const size_t frame_bytes = stride * panel_h;
    const size_t groups_per_row = memory_w >> 4;
    const size_t write_len = (memory_w >> 2) + me->config_detail().line_padding;
    const int magni_h = (me->_cfg.memory_height > me->_cfg.panel_height)
                      ? (me->_cfg.memory_height / me->_cfg.panel_height) : 1;
    const size_t scan_rows = (me->_cfg.memory_height + magni_h - 1) / magni_h;
    auto bus = me->getBusEPD2D();

    ESP_LOGI(TAG_2D,
             "task started: rows=%u groups=%u write_len=%u temperature=%dC",
             (unsigned)scan_rows, (unsigned)groups_per_row,
             (unsigned)write_len, me->_temperature_c);

    update_data_t update;
    for (;;) {
      me->_display_busy_2d = false;
      if (xQueueReceive(me->_update_queue_2d, &update, portMAX_DELAY) != pdTRUE) {
        continue;
      }
      // Queue send already wakes this task immediately; measure the actual
      // handoff separately from snapshot work and report only after scanning.
      const int64_t received_at_us = esp_timer_get_time();
      const int64_t queue_wait_us = received_at_us - update.queued_at_us;
      me->_display_busy_2d = true;
      xSemaphoreTake(me->_stats_mutex, portMAX_DELAY);
      StatsLock stats_lock{me->_stats_mutex};
      // Every submitted batch invalidates the prior physical generation,
      // including failed scans and menus. Cache matching uses the old value.
      const uint64_t previous_generation = me->_physical_generation++;
      bool prepared_stats_hit = false;
      unsigned prepared_stats_miss = 10; // Statistics cache not checked on this path.

      // Coalesce everything already pending into one stable framebuffer batch.
      update_data_t pending;
      unsigned merged = 0;
      while (xQueueReceive(me->_update_queue_2d, &pending, 0) == pdTRUE) {
        const uint32_t left = std::min<uint32_t>(update.x, pending.x);
        const uint32_t top = std::min<uint32_t>(update.y, pending.y);
        const uint32_t right = std::max<uint32_t>(uint32_t(update.x) + update.w,
                                                   uint32_t(pending.x) + pending.w);
        const uint32_t bottom = std::max<uint32_t>(uint32_t(update.y) + update.h,
                                                    uint32_t(pending.y) + pending.h);
        update.x = left;
        update.y = top;
        update.w = right - left;
        update.h = bottom - top;
        if (pending.mode < update.mode) {
          update.mode = pending.mode;
        }
        ++merged;
      }
      // Coalesced UI updates no longer describe one immutable page turn.
      if (merged) {
        update.animation_direction = 0;
        // Mixed UI batches must retain normal waveform semantics.
        update.changed_pixels_only = false;
      }

      if (!me->_physical_state_known) {
        update.changed_pixels_only = false;
        if (update.mode != epd_mode_t::epd_quality) {
          ESP_LOGW(TAG_2D, "physical state unknown; promoting first update to Quality");
        }
        update.mode = epd_mode_t::epd_quality;
        // Only recovery from unknown physical state requires a full refresh.
        // GC16/HD otherwise honor the caller's dirty rectangle.
        update.x = 0;
        update.y = 0;
        update.w = panel_w;
        update.h = panel_h;
      }

      // Only explicit HD feedback may suppress same-gray drives. In particular,
      // never alter normal HD, Quality recovery, or the tuned GC16 0->0 path.
      update.changed_pixels_only = update.changed_pixels_only
                               && update.mode == epd_mode_t::epd_hd;

      // Write back the sprite rows this batch is about to consume. This has to
      // precede the copy below so the PSRAM contents are coherent for anything
      // that reads them without going through this core's cache.
      //
      // It used to run in display(), i.e. on the application's core (core 0 -
      // the same one the USB host ISR and the usb_host task run on), and moved
      // here to keep a 300 us PSRAM walk off that core. That move was tested and
      // did NOT stop the "TX URB: 1/N packets skipped, first=0" reports, so the
      // stall being chased is not this walk occupying core 0. It stays here
      // anyway: the scan core is the better place for it, and the ordering
      // requirement below is unchanged.
      const int64_t snapshot_begin_us = esp_timer_get_time();
      {
        const size_t wb_top = std::min<size_t>(update.y, panel_h);
        const size_t wb_rows = std::min<size_t>(update.h, panel_h - wb_top);
        cache_writeback_2d(me->_buf + wb_top * stride, wb_rows * stride);
      }
      memcpy(me->_target_buf, me->_buf, frame_bytes);
      const int64_t snapshot_us = esp_timer_get_time() - snapshot_begin_us;
      const selected_waveform_t selected = me->selectWaveform(update.mode);
      if (selected.phases == nullptr || selected.phases->luts == nullptr
       || selected.phases->phases <= 0) {
        ESP_LOGE(TAG_2D, "no waveform for display mode %u", (unsigned)update.mode);
        me->_physical_state_known = false;
        continue;
      }
      bool animation_requested = false;
      // A missing HD table may fall back to another waveform. Keep that
      // waveform's normal same-gray compensation as well.
      update.changed_pixels_only = update.changed_pixels_only
                               && selected.type == WAVEFORM_MODE_HD;
#if (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31)) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN \
 && MYPAPER_EPD_2D_PRECOMPUTE_WAVEFORM
      animation_requested = update.animation_direction != 0 && me->_physical_state_known &&
          selected.type == WAVEFORM_MODE_DU4 &&
          (selected.phases->phases == 15 || selected.phases->phases == 16
           || selected.phases->phases == 18) &&
          panel_w == memory_w && scan_rows == panel_h &&
          me->_cfg.offset_x == 0 && me->_cfg.offset_y == 0 &&
          panel_w%16 == 0 && update.animation_rotation<8 &&
          lgfx_epd_page_turn::valid_direction(update.animation_direction);
#endif
      if (animation_requested) {
        update.x = 0; update.y = 0; update.w = panel_w; update.h = panel_h;
      }

      // Both binary waveforms consume the same quantized target used later
      // for transition selection and physical-state commit. Preserve _buf.
      if (selected.type == WAVEFORM_MODE_DU || selected.type == WAVEFORM_MODE_A2) {
        quantize_binary_target_rect_4bpp(
            me->_target_buf, stride, panel_w, panel_h,
            update.x, update.y, update.w, update.h);
        ESP_LOGI(TAG_2D,
                 "%s target quantized: source_levels=16 target_levels=2 threshold=8",
                 selected.type == WAVEFORM_MODE_DU ? "DU" : "A2");
      }

      ESP_LOGI(TAG_2D,
               "batch begin: mode=%u waveform_type=%u phases=%d temp_range=%u area=%u,%u %ux%u merged=%u",
               (unsigned)update.mode, (unsigned)selected.type,
               selected.phases->phases, (unsigned)selected.temperature_range,
               update.x, update.y, update.w, update.h, merged);
      if (selected.phases->phase_times != nullptr) {
        ESP_LOGW(TAG_2D,
                 "phase_times present; fixed full-panel scan timing is used in this validation engine");
      }
#if MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES > 0
      if (selected.type == WAVEFORM_MODE_GC16
       && selected.phases->phases == 48) {
        ESP_LOGI(TAG_2D,
                 "GC16 0->0 black compensation enabled: frames=%d final_idle=1",
                 MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES);
      }
#endif

      uint8_t transition[256];
      uint8_t* phase_frames = nullptr;
      uint16_t* phase_frame_map = nullptr;
      uint16_t* representative_phases = nullptr;
      uint8_t* unique_transitions = nullptr;
      bool phase_frames_temporary = false;
      int cached_phase_count = selected.phases->phases;
      bool deduplicated = false;
      bool gc16_pipeline_active = false;
      gc16_phase_pipeline_t gc16_pipeline = {};
      // Only task_update owns batches; workers finish before reuse. Keep the
      // bounded record array off the already busy display task stack.
      static drive_frame_timing_t frame_timings[GC16_PIPELINE_MAX_PHASES];
      for (auto& timing : frame_timings) timing = drive_frame_timing_t{};
      gc16_pipeline.timings = frame_timings;
      const uint16_t* pair_indices = nullptr;
      uint64_t pair_indices_us = 0;
      bool du4_pipeline_active = false;
      uint8_t* animation_frames = nullptr;
      int sequence_phases = selected.phases->phases;
      du4_phase_pipeline_t du4_pipeline = {};
      du4_pipeline.timings = frame_timings;
      animation_pipeline_t animation_pipeline = {};
      bool animation_deferred_source = false;
      // Capture only on the power-on critical path; report after power-off
      // and physical-state commit. In particular, preserve prime-only timing
      // before the asynchronous producer updates its render counters.
      struct {
        bool valid = false;
        bool fused = false;
        bool reused = false;
        bool triple_prime = false;
        uint64_t stats = 0, dedup = 0, workspace = 0, active = 0;
        uint64_t prime = 0, worker = 0, precompute = 0;
        uint64_t prime_lut = 0, prime_clear = 0, prime_pixels = 0;
        size_t groups = 0, total_groups = 0, transitions = 0, bytes = 0;
        int slots = 0, primed = 0;
      } du4_prep;
#if (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31)) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN \
 && MYPAPER_EPD_2D_PRECOMPUTE_WAVEFORM
      // DU skips the whole-phase cache only while the experimental line
      // streamer is enabled.  When streaming is disabled, Fast must allocate
      // the same precomputed cache as every other waveform mode.
      const bool needs_phase_cache =
#if MYPAPER_EPD_2D_LINE_STREAM
          selected.type != WAVEFORM_MODE_DU;
#else
          true;
#endif
      if (needs_phase_cache) {
        const int phase_count = selected.phases->phases;
        size_t used_transition_count = 256;
        const int64_t stats_begin = esp_timer_get_time();
        uint64_t stats_us = 0;
        uint64_t dedup_us = 0;
        uint64_t active_us = 0;
        size_t fused_active_count = 0;
        bool fused_stats = false;

#if MYPAPER_EPD_2D_DU_PHASE_DEDUP
        if (selected.type == WAVEFORM_MODE_GC16
         || selected.type == WAVEFORM_MODE_DU
         || selected.type == WAVEFORM_MODE_A2
         || selected.type == WAVEFORM_MODE_DU4
         || selected.type == WAVEFORM_MODE_HD) {
          phase_frame_map = static_cast<uint16_t*>(heap_caps_malloc(
              size_t(phase_count) * sizeof(uint16_t), MALLOC_CAP_8BIT));
          representative_phases = static_cast<uint16_t*>(heap_caps_malloc(
              size_t(phase_count) * sizeof(uint16_t), MALLOC_CAP_8BIT));
          unique_transitions = static_cast<uint8_t*>(heap_caps_malloc(
              size_t(phase_count) * 256, MALLOC_CAP_8BIT));
          if (phase_frame_map != nullptr && representative_phases != nullptr
           && unique_transitions != nullptr) {
            bool used_transitions[256];
            const size_t total_groups = (memory_w >> 4) * scan_rows;
            prepared_stats_miss = lgfx_epd_stats::prepared_miss_reason(
                me->_prepared_stats.valid, me->_physical_state_known,
                me->_prepared_stats.key,
                {previous_generation, update.animation_rotation, selected.type, selected.phases},
                update.x == 0 && update.y == 0 && update.w == panel_w && update.h == panel_h,
                me->_prepared_stats.target, me->_target_buf, frame_bytes);
            if (update.changed_pixels_only) prepared_stats_miss = 11;
            if (prepared_stats_miss == 0) {
              memcpy(used_transitions, me->_prepared_stats.used, sizeof(used_transitions));
              used_transition_count = me->_prepared_stats.transition_count;
              if (me->_prepared_stats.fused && me->_du4_active_groups_2d
               && me->_du4_active_group_capacity_2d >= total_groups) {
                fused_active_count = me->_prepared_stats.group_count;
                memcpy(me->_du4_active_groups_2d, me->_prepared_stats.groups,
                       fused_active_count * sizeof(uint16_t));
                fused_stats = true;
              }
              prepared_stats_hit = true;
            } else if (selected.type == WAVEFORM_MODE_DU4
             && update.x == 0 && update.y == 0
             && update.w == panel_w && update.h == panel_h
             && panel_w == memory_w && scan_rows == panel_h
             && memory_w % 16 == 0
             && me->_du4_active_groups_2d != nullptr
             && me->_du4_active_group_capacity_2d >= total_groups
             && total_groups <= UINT16_MAX) {
              bool driven[256];
              build_driven_transition_mask(driven, selected.phases, selected.type);
              used_transition_count = lgfx_epd_stats::collect(
                  used_transitions, me->_du4_active_groups_2d, fused_active_count,
                  me->_physical_buf, me->_target_buf, stride * panel_h, driven);
              fused_stats = true;
            } else {
              used_transition_count = collect_used_transitions_4bpp(
                  used_transitions, me->_physical_buf, me->_target_buf,
                  stride, panel_h,
                  update.x, update.y, update.w, update.h);
            }
            me->_prepared_stats.valid = false;
            stats_us = esp_timer_get_time() - stats_begin;
            const int64_t dedup_begin = esp_timer_get_time();
            cached_phase_count = 0;
            for (int phase = 0; phase < phase_count; ++phase) {
              const uint8_t* phase_lut = selected.phases->luts
                                       + size_t(phase) * 64;
              build_transition_lut(transition, phase_lut);
              apply_gc16_transition_adjustments(
                  transition, selected.type, phase, phase_count, update.changed_pixels_only);

              int match = -1;
              for (int unique = 0; unique < cached_phase_count; ++unique) {
                if (transitions_equal_for_used(
                        transition, unique_transitions + size_t(unique) * 256,
                        used_transitions)) {
                  match = unique;
                  break;
                }
              }
              if (match < 0) {
                match = cached_phase_count++;
                representative_phases[match] = uint16_t(phase);
                memcpy(unique_transitions + size_t(match) * 256,
                       transition, 256);
              }
              phase_frame_map[phase] = uint16_t(match);
            }
            deduplicated = true;
            dedup_us = esp_timer_get_time() - dedup_begin;
          } else {
            ESP_LOGW(TAG_2D,
                     "DU phase dedup metadata allocation failed; using full phase cache");
            heap_caps_free(phase_frame_map);
            heap_caps_free(representative_phases);
            heap_caps_free(unique_transitions);
            phase_frame_map = nullptr;
            representative_phases = nullptr;
            unique_transitions = nullptr;
          }
        }
#endif

        const int64_t workspace_begin = esp_timer_get_time();
        const bool gc16_pair_renderer =
#if MYPAPER_EPD_2D_GC16_PAIR_LUT
            (selected.type == WAVEFORM_MODE_GC16
          || selected.type == WAVEFORM_MODE_HD)
         && me->_gc16_pair_lut_2d != nullptr
         && panel_w == memory_w && scan_rows == panel_h;
#else
            false;
#endif
        // DU/A2 use the same arbitrary 16x16 transition mapping. Reuse the
        // internal 64 KiB pair LUT, but keep their full precompute policy;
        // selecting this renderer must not enable the GC16 ring pipeline.
        const bool binary_pair_renderer =
#if MYPAPER_EPD_2D_GC16_PAIR_LUT
            (selected.type == WAVEFORM_MODE_DU || selected.type == WAVEFORM_MODE_A2)
         && me->_gc16_pair_lut_2d != nullptr
         && panel_w == memory_w && scan_rows == panel_h;
#else
            false;
#endif
        const int gc16_pipeline_prime = selected.type == WAVEFORM_MODE_HD
            ? MYPAPER_EPD_2D_HD_PIPELINE_PRIME
            : MYPAPER_EPD_2D_GC16_PIPELINE_PRIME;
        // Use the configured prime count even for dense GC16 images. Paired
        // background generation now uses the remaining ring slots; do not
        // silently increase preheat to the full ring when unique_count > 32.
        // Keep support for a configured full-ring preheat as an A/B fallback.
        const bool gc16_pipeline_can_fill_ring = true;
        const bool gc16_pipeline_requested =
#if MYPAPER_EPD_2D_GC16_PIPELINE
            gc16_pair_renderer && deduplicated
         && cached_phase_count > gc16_pipeline_prime
         && cached_phase_count <= GC16_PIPELINE_MAX_PHASES
         && MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS
                <= GC16_PIPELINE_MAX_SLOTS
         && gc16_pipeline_prime > 0
         && (gc16_pipeline_can_fill_ring
               ? gc16_pipeline_prime <= MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS
               : gc16_pipeline_prime < MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS)
         && uxTaskPriorityGet(nullptr) > 1;
#else
            false;
#endif
        const bool du4_pipeline_requested =
#if MYPAPER_EPD_2D_DU4_PIPELINE
            selected.type == WAVEFORM_MODE_DU4
         && !animation_requested
         && me->_du4_pair_lut_2d != nullptr
         && deduplicated
         && cached_phase_count > MYPAPER_EPD_2D_DU4_PIPELINE_PRIME
         && cached_phase_count <= DU4_PIPELINE_MAX_PHASES
         && MYPAPER_EPD_2D_DU4_PIPELINE_PRIME > 0
         && uxTaskPriorityGet(nullptr) > 1;
#else
            false;
#endif
        const int phase_storage_count = gc16_pipeline_requested
            ? std::min<int>(MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS,
                            cached_phase_count)
            : cached_phase_count;
        const size_t phase_frame_bytes = me->_scan_frame_bytes_2d;
        const size_t phase_cache_bytes = phase_frame_bytes
                                       * size_t(phase_storage_count);
        bool du4_workspace_reused = false;
#if defined(CONFIG_IDF_TARGET_ESP32S31)
        if (me->_phase_arena_2d && phase_cache_bytes <= me->_phase_arena_capacity_2d) {
          phase_frames = me->_phase_arena_2d;
          du4_workspace_reused = selected.type == WAVEFORM_MODE_DU4;
          // Not temporary: only the panel's refresh task borrows this arena.
          // It must never enter DU4's evictable cache or per-batch free path.
        }
#endif
        if (selected.type == WAVEFORM_MODE_DU4 && phase_frames == nullptr) {
          if (me->_du4_phase_frames_2d == nullptr
           || me->_du4_phase_frame_capacity_2d < phase_cache_bytes) {
            uint8_t* expanded = static_cast<uint8_t*>(heap_caps_aligned_alloc(
                64, phase_cache_bytes,
                MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
            if (expanded != nullptr) {
              heap_caps_free(me->_du4_phase_frames_2d);
              me->_du4_phase_frames_2d = expanded;
              me->_du4_phase_frame_capacity_2d = phase_cache_bytes;
            }
          } else {
            du4_workspace_reused = true;
          }
          if (me->_du4_phase_frame_capacity_2d >= phase_cache_bytes) {
            phase_frames = me->_du4_phase_frames_2d;
          }
        }
        if (phase_frames == nullptr) {
          phase_frames = static_cast<uint8_t*>(heap_caps_aligned_alloc(
              64, phase_cache_bytes,
              MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
          phase_frames_temporary = phase_frames != nullptr;
        }
        if (phase_frames == nullptr && selected.type != WAVEFORM_MODE_DU4
         && me->_du4_phase_frames_2d != nullptr) {
          ESP_LOGW(TAG_2D,
                   "temporary phase cache needs %u bytes; releasing %u-byte DU4 workspace and retrying",
                   (unsigned)phase_cache_bytes,
                   (unsigned)me->_du4_phase_frame_capacity_2d);
          heap_caps_free(me->_du4_phase_frames_2d);
          me->_du4_phase_frames_2d = nullptr;
          me->_du4_phase_frame_capacity_2d = 0;
          phase_frames = static_cast<uint8_t*>(heap_caps_aligned_alloc(
              64, phase_cache_bytes,
              MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
          phase_frames_temporary = phase_frames != nullptr;
        }
        if (phase_frames == nullptr) {
          ESP_LOGE(TAG_2D,
                   "phase cache allocation failed: phases=%d cached=%d bytes=%u psram_free=%u psram_largest=%u; batch aborted to preserve waveform timing",
                   phase_count, cached_phase_count,
                   (unsigned)phase_cache_bytes,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
          heap_caps_free(phase_frame_map);
          heap_caps_free(representative_phases);
          heap_caps_free(unique_transitions);
          me->_physical_state_known = false;
          continue;
        }

        // Build only from this batch's immutable target/physical snapshots.
        // Do not cache validity across batches or alter the calibrated waveform.
        // Prioritize scan-frame allocation; inability to reserve indices falls back.
#if MYPAPER_EPD_2D_GC16_PAIR_INDICES
        if (gc16_pair_renderer && cached_phase_count >= 3) {
          const size_t count = (memory_w >> 1) * scan_rows;
          if (me->_gc16_pair_indices_capacity_2d < count) {
            auto* expanded = static_cast<uint16_t*>(heap_caps_aligned_alloc(
                64, count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (expanded) {
              heap_caps_free(me->_gc16_pair_indices_2d);
              me->_gc16_pair_indices_2d = expanded;
              me->_gc16_pair_indices_capacity_2d = count;
            }
          }
          if (me->_gc16_pair_indices_capacity_2d >= count) {
            const int64_t begin = esp_timer_get_time();
            const size_t index_stride = memory_w >> 1;
            lgfx_epd_pair_indices::build_rows(me->_gc16_pair_indices_2d,
                me->_physical_buf, me->_target_buf, index_stride,
                std::min(index_stride, size_t(update.x) >> 1),
                std::min(index_stride, (size_t(update.x) + update.w) >> 1),
                std::min(scan_rows, size_t(update.y)),
                std::min(scan_rows, size_t(update.y) + update.h));
            pair_indices_us = esp_timer_get_time() - begin;
            pair_indices = me->_gc16_pair_indices_2d;
          }
        }
#endif
        gc16_pipeline.indices = pair_indices;
        if (gc16_pipeline_requested) {
          gc16_pipeline_active = true;
          gc16_pipeline.frames = phase_frames;
          gc16_pipeline.frame_bytes = phase_frame_bytes;
          gc16_pipeline.physical = me->_physical_buf;
          gc16_pipeline.target = me->_target_buf;
          gc16_pipeline.pair_lut = me->_gc16_pair_lut_2d;
          gc16_pipeline.unique_transitions = unique_transitions;
          gc16_pipeline.memory_width = memory_w;
          gc16_pipeline.scan_rows = scan_rows;
          gc16_pipeline.update_x = update.x;
          gc16_pipeline.update_y = update.y;
          gc16_pipeline.update_width = update.w;
          gc16_pipeline.update_height = update.h;
          gc16_pipeline.unique_count = cached_phase_count;
          gc16_pipeline.slot_count = phase_storage_count;
          gc16_pipeline.next_unique = std::min<int>(
              gc16_pipeline_prime, cached_phase_count);
          gc16_pipeline.done = xSemaphoreCreateBinary();
          for (int slot = 0; slot < GC16_PIPELINE_MAX_SLOTS; ++slot) {
            gc16_pipeline.ready_unique[slot] = -1;
          }
          for (int unique = 0; unique < cached_phase_count; ++unique) {
            gc16_pipeline.unique_slot[unique] = -1;
            gc16_pipeline.last_phase[unique] = 0;
          }
          for (int phase = 0; phase < phase_count; ++phase) {
            gc16_pipeline.last_phase[phase_frame_map[phase]] = uint16_t(phase);
          }
          if (gc16_pipeline.done == nullptr) {
            ESP_LOGE(TAG_2D,
                     "GC16 pipeline semaphore allocation failed");
            if (phase_frames_temporary) {
              heap_caps_free(phase_frames);
            }
            heap_caps_free(phase_frame_map);
            heap_caps_free(representative_phases);
            heap_caps_free(unique_transitions);
            me->_physical_state_known = false;
            continue;
          }
        }

        const int64_t precompute_begin = esp_timer_get_time();
        const uint64_t workspace_us = precompute_begin - workspace_begin;
        uint64_t pair_lut_build_us = 0;
        uint64_t phase_render_us = 0;
        uint64_t du4_prime_lut_us = 0;
        du4_render_timing_t du4_prime_timing;
        const size_t total_group_count = (memory_w >> 4) * scan_rows;
        size_t du4_active_group_count = total_group_count;
        const uint16_t* du4_active_groups = nullptr;
        const bool du4_full_screen = selected.type == WAVEFORM_MODE_DU4
                                  && update.x == 0 && update.y == 0
                                  && update.w == panel_w
                                  && update.h == panel_h
                                  && panel_w == memory_w
                                  && scan_rows == panel_h;
        if (du4_full_screen && me->_du4_active_groups_2d != nullptr) {
          const int64_t active_begin = esp_timer_get_time();
          if (fused_stats) {
            du4_active_group_count = fused_active_count;
          } else {
          bool driven_transitions[256];
          build_driven_transition_mask(
              driven_transitions, selected.phases, selected.type);
          du4_active_group_count = collect_active_groups_fullscreen(
              me->_du4_active_groups_2d,
              me->_du4_active_group_capacity_2d,
              me->_physical_buf, me->_target_buf,
              stride, panel_h, driven_transitions);
          }
          active_us = esp_timer_get_time() - active_begin;
          // Clearing a complete frame is cheap, but sparse indexing stops
          // paying off when almost every 16-pixel group still needs work.
          if (du4_active_group_count * 8u < total_group_count * 7u) {
            du4_active_groups = me->_du4_active_groups_2d;
          }
        }
#if MYPAPER_EPD_2D_DU4_PAIR_INDICES
        if (du4_full_screen && deduplicated && cached_phase_count >= 3 && me->_du4_pair_lut_2d) {
          // Reuse the GC16 PSRAM allocation, but rebuild with DU4 12-bit keys.
          // No content validity is carried between batches or waveform types.
          const size_t count = (memory_w >> 1) * scan_rows;
          if (me->_gc16_pair_indices_capacity_2d < count) {
            auto* expanded = static_cast<uint16_t*>(heap_caps_aligned_alloc(
                64, count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (expanded) {
              heap_caps_free(me->_gc16_pair_indices_2d);
              me->_gc16_pair_indices_2d = expanded;
              me->_gc16_pair_indices_capacity_2d = count;
            }
          }
          if (me->_gc16_pair_indices_capacity_2d >= count) {
            const int64_t begin = esp_timer_get_time();
            const size_t groups = du4_active_groups ? du4_active_group_count : total_group_count;
            for (size_t first = 0; first < groups;) {
              const size_t chunk = std::min<size_t>(256, groups - first);
              const size_t offset = du4_active_groups ? 0 : first << 3;
              lgfx_epd_du4::build_indices(me->_gc16_pair_indices_2d + offset,
                  me->_physical_buf + offset, me->_target_buf + offset,
                  me->_du4_pair_lut_2d + DU4_PAIR_LUT_BYTES,
                  chunk, du4_active_groups ? du4_active_groups + first : nullptr, chunk);
              first += chunk;
              if (first < groups) du4_bus_relief();
            }
            pair_indices_us = esp_timer_get_time() - begin;
            pair_indices = me->_gc16_pair_indices_2d;
          }
        }
#endif
        animation_deferred_source = animation_requested && deduplicated &&
            me->_du4_pair_lut_2d != nullptr && cached_phase_count <= DU4_PIPELINE_MAX_PHASES;
        if (du4_pipeline_requested || animation_deferred_source) {
          du4_pipeline.indices = pair_indices;
          du4_pipeline.frames = phase_frames;
          du4_pipeline.frame_bytes = phase_frame_bytes;
          du4_pipeline.physical = me->_physical_buf;
          du4_pipeline.target = me->_target_buf;
          du4_pipeline.pair_lut = me->_du4_pair_lut_2d;
          du4_pipeline.unique_transitions = unique_transitions;
          du4_pipeline.panel_width = panel_w;
          du4_pipeline.panel_height = panel_h;
          du4_pipeline.memory_width = memory_w;
          du4_pipeline.scan_rows = scan_rows;
          du4_pipeline.update_x = update.x;
          du4_pipeline.update_y = update.y;
          du4_pipeline.update_width = update.w;
          du4_pipeline.update_height = update.h;
          du4_pipeline.active_groups = du4_active_groups;
          du4_pipeline.active_group_count = du4_active_group_count;
          du4_pipeline.unique_count = cached_phase_count;
          du4_pipeline.next_unique = std::min<int>(
              MYPAPER_EPD_2D_DU4_PIPELINE_PRIME, cached_phase_count);
          if (du4_pipeline_requested) {
          du4_pipeline.done = xSemaphoreCreateBinary();
          if (du4_pipeline.done == nullptr) {
            ESP_LOGW(TAG_2D,
                     "DU4 pipeline semaphore allocation failed; using full precompute");
          } else {
            du4_pipeline_active = true;
          }
          }
          if (animation_deferred_source) du4_pipeline.next_unique = 0;
        }
        const int initial_render_count = gc16_pipeline_active
            ? gc16_pipeline.next_unique
            : du4_pipeline_active ? int(du4_pipeline.next_unique)
                                  : animation_deferred_source ? 0 : cached_phase_count;
        int rendered_prime_count = 0;
        for (int cached = 0; cached < std::min(cached_phase_count, GC16_PIPELINE_MAX_PHASES); ++cached)
          frame_timings[cached].phase = deduplicated ? representative_phases[cached] : cached;
#if MYPAPER_EPD_2D_DU4_TRIPLE_PRIME
        if (du4_pipeline_active && du4_full_screen && (du4_active_groups || pair_indices) && deduplicated
         && initial_render_count == 3 && me->_du4_pair_lut_2d) {
          if (!me->_du4_prime_pair_luts_2d)
            me->_du4_prime_pair_luts_2d = static_cast<uint8_t*>(heap_caps_malloc(
                2 * DU4_PAIR_LUT_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
          if (me->_du4_prime_pair_luts_2d) {
            const int64_t render_begin = esp_timer_get_time();
            uint8_t* lut0 = me->_du4_pair_lut_2d;
            uint8_t* lut1 = me->_du4_prime_pair_luts_2d;
            uint8_t* lut2 = lut1 + DU4_PAIR_LUT_BYTES;
            build_du4_pair_lut(lut0, unique_transitions);
            build_du4_pair_lut(lut1, unique_transitions + 256);
            build_du4_pair_lut(lut2, unique_transitions + 512);
            du4_prime_lut_us += esp_timer_get_time() - render_begin;
            auto& detail = frame_timings[0];
            detail.lut_us = esp_timer_get_time() - render_begin;
            detail.count = 3;
            detail.prime = true;
            if (pair_indices) {
              const uint8_t* luts[] = {lut0, lut1, lut2};
              render_du4_index_frames<3>(phase_frames, phase_frame_bytes, pair_indices,
                  total_group_count, du4_active_groups, du4_active_group_count,
                  luts, du4_prime_timing);
            } else {
            render_du4_three_frames(phase_frames, phase_frame_bytes,
                me->_physical_buf, me->_target_buf, total_group_count,
                du4_active_groups, du4_active_group_count,
                lut0 + DU4_PAIR_LUT_BYTES, lut0, lut1, lut2, du4_prime_timing);
            }
            detail.clear_us = du4_prime_timing.clear_us;
            detail.pixels_us = du4_prime_timing.pixels_us;
            detail.total_us = esp_timer_get_time() - render_begin;
            phase_render_us += esp_timer_get_time() - render_begin;
            for (int cached = 0; cached < 3; ++cached)
              __atomic_store_n(&du4_pipeline.ready[cached], true, __ATOMIC_RELEASE);
            rendered_prime_count = 3;
            du4_prep.triple_prime = true;
          }
        }
#endif
        // Pair preheat in the already allocated consecutive ring slots.
#if MYPAPER_EPD_2D_GC16_DUAL_PRIME
        if (gc16_pair_renderer && pair_indices && deduplicated
         && initial_render_count <= GC16_PIPELINE_MAX_PHASES) {
          for (; rendered_prime_count + 1 < initial_render_count; rendered_prime_count += 2) {
            const int cached = rendered_prime_count;
            auto& detail = frame_timings[cached];
            const int64_t begin = esp_timer_get_time();
            lgfx_epd_gc16_dual::build_lut(me->_gc16_pair_lut_2d,
                unique_transitions + size_t(cached) * 256,
                unique_transitions + size_t(cached + 1) * 256);
            const int64_t lut_end = esp_timer_get_time();
            detail.lut_us = lut_end - begin;
            pair_lut_build_us += detail.lut_us;
            auto* first = phase_frames + size_t(cached) * phase_frame_bytes;
            auto* second = first + phase_frame_bytes;
            const bool partial = update.x != 0 || update.y != 0
                || update.w != memory_w || update.h != scan_rows;
            if (partial) {
              memset(first, 0, phase_frame_bytes);
              memset(second, 0, phase_frame_bytes);
            }
            const int64_t clear_end = partial ? esp_timer_get_time() : lut_end;
            detail.clear_us = clear_end - lut_end;
            lgfx_epd_gc16_dual::render<MYPAPER_EPD_OUTPUT_PERMUTE == 1,true>(
                first, second, me->_physical_buf, me->_target_buf, pair_indices,
                me->_gc16_pair_lut_2d, memory_w, scan_rows,
                update.x, update.y, update.w, update.h, true);
            const int64_t end = esp_timer_get_time();
            detail.pixels_us = end - clear_end;
            detail.total_us = end - begin;
            detail.count = 2;
            detail.prime = true;
            phase_render_us += end - lut_end;
            if (gc16_pipeline_active) {
              for (int i = cached; i < cached + 2; ++i) {
                __atomic_store_n(&gc16_pipeline.unique_slot[i], i, __ATOMIC_RELEASE);
                __atomic_store_n(&gc16_pipeline.ready_unique[i], i, __ATOMIC_RELEASE);
              }
            }
          }
        }
#endif
        int64_t preparation_yield_at = esp_timer_get_time();
        for (int cached = rendered_prime_count; cached < initial_render_count; ++cached) {
          drive_frame_timing_t* detail = cached < GC16_PIPELINE_MAX_PHASES
              ? &frame_timings[cached] : nullptr;
          const int64_t frame_begin = esp_timer_get_time();
          if (detail) detail->prime = true;
          const int phase = deduplicated ? representative_phases[cached]
                                         : cached;
          const uint8_t* render_transition = nullptr;
          if (deduplicated) {
            render_transition = unique_transitions + size_t(cached) * 256;
          } else {
            const uint8_t* phase_lut = selected.phases->luts
                                     + size_t(phase) * 64;
            build_transition_lut(transition, phase_lut);
            apply_gc16_transition_adjustments(
                transition, selected.type, phase, phase_count, update.changed_pixels_only);
            render_transition = transition;
          }
          if (gc16_pair_renderer || binary_pair_renderer) {
            const int64_t lut_begin = esp_timer_get_time();
            build_gc16_pair_lut(
                me->_gc16_pair_lut_2d, render_transition);
            pair_lut_build_us += esp_timer_get_time() - lut_begin;
            const int64_t render_begin = esp_timer_get_time();
            if (detail) detail->lut_us = render_begin - lut_begin;
            render_gc16_pair_phase_frame(
                phase_frames + size_t(cached) * phase_frame_bytes,
                me->_physical_buf, me->_target_buf,
                memory_w, scan_rows, me->_gc16_pair_lut_2d,
                update.x, update.y, update.w, update.h, detail, pair_indices);
            phase_render_us += esp_timer_get_time() - render_begin;
          } else if (selected.type == WAVEFORM_MODE_DU4
           && me->_du4_pair_lut_2d != nullptr) {
            const int64_t render_begin = esp_timer_get_time();
            build_du4_pair_lut(me->_du4_pair_lut_2d, render_transition);
            const int64_t lut_end = esp_timer_get_time();
            du4_render_timing_t frame_detail;
            if (detail) detail->lut_us = lut_end - render_begin;
            if (du4_pipeline_active)
              du4_prime_lut_us += esp_timer_get_time() - render_begin;
            if (pair_indices) {
              const uint8_t* luts[] = {me->_du4_pair_lut_2d};
              render_du4_index_frames<1>(phase_frames + size_t(cached) * phase_frame_bytes,
                  phase_frame_bytes, pair_indices, total_group_count,
                  du4_active_groups, du4_active_group_count, luts, frame_detail);
            } else {
            render_du4_phase_frame(
                phase_frames + size_t(cached) * phase_frame_bytes,
                phase_frame_bytes, me->_physical_buf, me->_target_buf,
                panel_w, panel_h, memory_w, scan_rows,
                update.x, update.y, update.w, update.h,
                me->_du4_pair_lut_2d,
                me->_du4_pair_lut_2d + DU4_PAIR_LUT_BYTES,
                du4_equal_pairs_are_idle(render_transition),
                du4_active_groups, du4_active_group_count,
                &frame_detail);
            }
            if (du4_pipeline_active) {
              du4_prime_timing.clear_us += frame_detail.clear_us;
              du4_prime_timing.pixels_us += frame_detail.pixels_us;
            }
            if (detail) {
              detail->clear_us = frame_detail.clear_us;
              detail->pixels_us = frame_detail.pixels_us;
            }
            phase_render_us += esp_timer_get_time() - render_begin;
          } else {
            const int64_t render_begin = esp_timer_get_time();
            render_phase_frame(
                phase_frames + size_t(cached) * phase_frame_bytes,
                phase_frame_bytes, me->_physical_buf, me->_target_buf,
                panel_w, panel_h, memory_w, scan_rows,
                update.x, update.y, update.w, update.h, render_transition);
            phase_render_us += esp_timer_get_time() - render_begin;
          }
          if (detail) detail->total_us = esp_timer_get_time() - frame_begin;
          if (gc16_pipeline_active) {
            const int slot = cached;
            __atomic_store_n(&gc16_pipeline.unique_slot[cached], slot,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&gc16_pipeline.ready_unique[slot], cached,
                             __ATOMIC_RELEASE);
          }
          if (du4_pipeline_active) {
            __atomic_store_n(&du4_pipeline.ready[cached], true,
                             __ATOMIC_RELEASE);
          }
          // Only full precomputation is allowed to sleep. The panel is still
          // off here; never add waits between physical scan phases or in a
          // producer racing the scanner. taskYIELD alone cannot run IDLE1.
          if (!gc16_pipeline_active && !du4_pipeline_active &&
              esp_timer_get_time() - preparation_yield_at >= 50000) {
            vTaskDelay(1);
            preparation_yield_at = esp_timer_get_time();
          }
        }
        const uint32_t precompute_us = static_cast<uint32_t>(
            esp_timer_get_time() - precompute_begin);
        if (gc16_pipeline_active) {
          gc16_pipeline.lut_build_us = pair_lut_build_us;
          gc16_pipeline.frame_render_us = phase_render_us;
          const UBaseType_t current_priority = uxTaskPriorityGet(nullptr);
          const BaseType_t task_result = create_epd_pipeline_task(
              gc16_phase_producer_task, "epd2d_gc16", 4096,
              &gc16_pipeline, current_priority - 1,
              &gc16_pipeline.task, xPortGetCoreID(),
              &gc16_pipeline.external_stack);
          if (task_result != pdPASS) {
            ESP_LOGE(TAG_2D, "GC16 pipeline producer creation failed");
            vSemaphoreDelete(gc16_pipeline.done);
            gc16_pipeline.done = nullptr;
            if (phase_frames_temporary) {
              heap_caps_free(phase_frames);
            }
            heap_caps_free(phase_frame_map);
            heap_caps_free(representative_phases);
            heap_caps_free(unique_transitions);
            me->_physical_state_known = false;
            continue;
          }
          ESP_LOGI(TAG_2D,
                   "waveform pipeline primed before power-on: phases=%d cached=%d transitions=%u slots=%d primed=%d bytes=%u time=%lu.%03lu ms lut_build=%llu us frame_render=%llu us renderer=%s stack=%s",
                   phase_count, cached_phase_count,
                   (unsigned)used_transition_count, phase_storage_count,
                   initial_render_count, (unsigned)phase_cache_bytes,
                   (unsigned long)(precompute_us / 1000),
                   (unsigned long)(precompute_us % 1000),
                   (unsigned long long)pair_lut_build_us,
                   (unsigned long long)phase_render_us,
                   selected.type == WAVEFORM_MODE_HD
                       ? "hd-gl16-pair-lut-ring" : "gc16-pair-lut-ring",
                   gc16_pipeline.external_stack ? "psram" : "internal");
        } else if (du4_pipeline_active) {
          du4_pipeline.frame_render_us = phase_render_us;
          const int64_t worker_begin = esp_timer_get_time();
          const UBaseType_t current_priority = uxTaskPriorityGet(nullptr);
          const BaseType_t task_result = create_epd_pipeline_task(
              du4_phase_producer_task, "epd2d_du4", 4096,
              &du4_pipeline, current_priority - 1,
              &du4_pipeline.task, xPortGetCoreID(),
              &du4_pipeline.external_stack);
          if (task_result != pdPASS) {
            ESP_LOGE(TAG_2D, "DU4 pipeline producer creation failed");
            vSemaphoreDelete(du4_pipeline.done);
            du4_pipeline.done = nullptr;
            if (phase_frames_temporary) {
              heap_caps_free(phase_frames);
            }
            heap_caps_free(phase_frame_map);
            heap_caps_free(representative_phases);
            heap_caps_free(unique_transitions);
            me->_physical_state_known = false;
            continue;
          }
          const uint64_t worker_us = esp_timer_get_time() - worker_begin;
          du4_prep.valid = true;
          du4_prep.fused = fused_stats;
          du4_prep.reused = du4_workspace_reused;
          du4_prep.stats = stats_us;
          du4_prep.dedup = dedup_us;
          du4_prep.workspace = workspace_us;
          du4_prep.active = active_us;
          du4_prep.prime = phase_render_us;
          du4_prep.prime_lut = du4_prime_lut_us;
          du4_prep.prime_clear = du4_prime_timing.clear_us;
          du4_prep.prime_pixels = du4_prime_timing.pixels_us;
          du4_prep.worker = worker_us;
          du4_prep.precompute = precompute_us;
          du4_prep.groups = du4_active_group_count;
          du4_prep.total_groups = total_group_count;
          du4_prep.transitions = used_transition_count;
          du4_prep.bytes = phase_cache_bytes;
          du4_prep.slots = phase_storage_count;
          du4_prep.primed = initial_render_count;
        } else {
          ESP_LOGI(TAG_2D,
                   "waveform preparation: stage=%s phases=%d cached=%d transitions=%u bytes=%u time=%lu.%03lu ms lut_build=%llu us frame_render=%llu us renderer=%s workspace=%s active_groups=%u/%u path=%s",
                   animation_deferred_source ? "workspace" : "complete",phase_count, cached_phase_count,
                   (unsigned)used_transition_count, (unsigned)phase_cache_bytes,
                   (unsigned long)(precompute_us / 1000),
                   (unsigned long)(precompute_us % 1000),
                   (unsigned long long)pair_lut_build_us,
                   (unsigned long long)phase_render_us,
                   gc16_pair_renderer
                       ? (selected.type == WAVEFORM_MODE_HD
                           ? "hd-gl16-pair-lut" : "gc16-pair-lut")
                     : binary_pair_renderer
                       ? (selected.type == WAVEFORM_MODE_DU ? "du-pair-lut" : "a2-pair-lut")
                     : selected.type == WAVEFORM_MODE_DU4
                     && me->_du4_pair_lut_2d != nullptr
                       ? "du4-pair-lut" : "generic",
                   selected.type == WAVEFORM_MODE_DU4
                       ? (du4_workspace_reused ? "reused" : "grown")
                       : "temporary",
                   (unsigned)du4_active_group_count,
                   (unsigned)total_group_count,
                   du4_active_groups != nullptr ? "sparse" : "full");
          if (!animation_deferred_source) {
          heap_caps_free(representative_phases);
          representative_phases = nullptr;
          heap_caps_free(unique_transitions);
          unique_transitions = nullptr;
          }
        }
      }
#endif

      // Ordered dependency generation; only complete immutable animation frames
      // are exposed to the scanner. No concurrent writes to an active DMA frame.
      if (animation_requested && phase_frames != nullptr) {
        const int spread=update.animation_speed==2 ? lgfx_epd_page_turn::slow_spread : lgfx_epd_page_turn::fast_spread;
        const int band_count=update.animation_speed==2 ? 8 : 4;
        const int count = lgfx_epd_page_turn::phases(selected.phases->phases,spread);
        animation_pipeline.spread=spread;
        if (animation_deferred_source && uxTaskPriorityGet(nullptr)>1)
          animation_pipeline.done=xSemaphoreCreateBinary();
        animation_pipeline.slots=animation_pipeline.done ? std::min(12,count) : count;
        for (auto& ready : animation_pipeline.ready) ready=-1;
        size_t bytes = size_t(animation_pipeline.slots)*me->_scan_frame_bytes_2d;
        // S31's LCD GDMA can read aligned ordinary PSRAM directly, but its
        // PSRAM heap does not advertise MALLOC_CAP_DMA. The shared scanner
        // enforces alignment/cache writeback. P4 retains its existing caps.
#if defined(CONFIG_IDF_TARGET_ESP32S31)
        constexpr uint32_t animation_cache_caps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM;
#else
        constexpr uint32_t animation_cache_caps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
#endif
        const int64_t alloc_begin=esp_timer_get_time();
        animation_frames = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            64, bytes, animation_cache_caps));
        const int64_t alloc_done=esp_timer_get_time();
        const int direction=update.animation_direction;
        const int slot=direction>0 ? direction-1 : 4-direction-1;
        auto &layout=me->_animation_layouts_2d[slot];
        const bool layout_cached=layout.matches(memory_w,scan_rows,update.animation_rotation,direction,band_count);
        bool layout_ready=layout_cached;
        if (animation_frames && !layout_cached) {
          heap_caps_free(layout.rows); layout=lgfx_epd_page_turn::Layout{};
          auto *rows=(direction>2 || direction<-2) ?
              static_cast<lgfx_epd_page_turn::Span*>(heap_caps_malloc(
                  size_t(scan_rows)*band_count*sizeof(lgfx_epd_page_turn::Span),
                  MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM)) : nullptr;
          layout_ready=lgfx_epd_page_turn::prepare_layout(layout,memory_w,scan_rows,
              update.animation_rotation,direction,rows,band_count);
          if (!layout_ready) heap_caps_free(rows);
        }
        const int64_t layout_done=esp_timer_get_time();
        if (animation_frames != nullptr && layout_ready) {
          int64_t zero_us=0,copy_us=0;
          int sources_primed=0;
          uint64_t render_primed_us=0;
          int initial_count=count;
          if (animation_deferred_source) {
            animation_pipeline.source=&du4_pipeline;
            animation_pipeline.frames=animation_frames;
            animation_pipeline.map=phase_frame_map;
            animation_pipeline.layout=&layout;
            animation_pipeline.waveform_phases=selected.phases->phases;
            animation_pipeline.count=count;
            initial_count=animation_pipeline.done ? std::min(8,count) : count;
          }
          for (int phase=0; phase<initial_count; ++phase) {
            if (animation_deferred_source) {
              render_animation_phase(&animation_pipeline,phase);
              continue;
            }
            auto *out=animation_frames+size_t(phase)*me->_scan_frame_bytes_2d;
            const int64_t zero_begin=esp_timer_get_time();
            lgfx_epd_page_turn::clear_inactive_layout(out,me->_scan_frame_bytes_2d,
                selected.phases->phases,phase,layout,spread);
            const int64_t copy_begin=esp_timer_get_time();
            lgfx_epd_page_turn::copy_layout(out,me->_scan_frame_bytes_2d,phase_frames,phase_frame_map,
                selected.phases->phases,phase,layout,false,spread);
            const int64_t copy_done=esp_timer_get_time();
            zero_us+=copy_begin-zero_begin; copy_us+=copy_done-copy_begin;
          }
          if (animation_deferred_source) {
            // Fixed eight-frame prime; six frames underrun in device testing.
            // An underrun remains fatal
            // to the batch rather than stretching the waveform phase timing.
            initial_count=animation_pipeline.next;
            sources_primed=du4_pipeline.next_unique;
            render_primed_us=du4_pipeline.frame_render_us;
            // Snapshot prime-only counters before the producer can run.
            zero_us=animation_pipeline.zero_us; copy_us=animation_pipeline.copy_us;
            if (animation_pipeline.done && create_epd_pipeline_task(
                animation_producer_task,"epd2d_anim",4096,&animation_pipeline,
                uxTaskPriorityGet(nullptr)-1,&animation_pipeline.task,xPortGetCoreID(),
                &animation_pipeline.external_stack)!=pdPASS) {
              vSemaphoreDelete(animation_pipeline.done); animation_pipeline.done=nullptr;
              // No worker can drain a ring before power-on. Expand to a full
              // sequence, preserving already primed frames, or use normal DU4.
              bytes=size_t(count)*me->_scan_frame_bytes_2d;
              auto *full=static_cast<uint8_t*>(heap_caps_aligned_alloc(
                  64,bytes,animation_cache_caps));
              if (full) std::memcpy(full,animation_frames,
                  size_t(animation_pipeline.next)*me->_scan_frame_bytes_2d);
              heap_caps_free(animation_frames); animation_frames=full;
              animation_pipeline.frames=full;
              animation_pipeline.slots=count;
              if (full) {
                for (int phase=animation_pipeline.next;phase<count;++phase)
                  render_animation_phase(&animation_pipeline,phase);
                initial_count=count;
                sources_primed=du4_pipeline.next_unique;
                render_primed_us=du4_pipeline.frame_render_us;
                ESP_LOGW(TAG_2D,"animation producer unavailable; full sequence prepared before power-on");
              } else {
                for (int unique=0;unique<cached_phase_count;++unique)
                  if (!du4_pipeline.ready[unique]) render_du4_pipeline_unique(&du4_pipeline,unique);
                ESP_LOGW(TAG_2D,"animation producer and full cache unavailable; using normal DU4");
              }
              zero_us=animation_pipeline.zero_us; copy_us=animation_pipeline.copy_us;
            }
          }
          sequence_phases = animation_frames ? count : selected.phases->phases;
          if (animation_frames) {
          const int64_t prepare_us=esp_timer_get_time()-alloc_done;
          ESP_LOGI(TAG_2D, "page turn prepared: direction=%s speed=%s bands=%d spread=%d phases=%d bytes=%u prepare=%lld compose=%lld us alloc=%lld layout=%lld zero=%lld copy=%lld us layout_cache=%d path=%s zero_scope=inactive",
              lgfx_epd_page_turn::direction_name(direction),update.animation_speed==2 ? "slow" : "fast",band_count,spread,count,unsigned(bytes),
              (long long)prepare_us,(long long)(prepare_us-render_primed_us),(long long)(alloc_done-alloc_begin),
              (long long)(layout_done-alloc_done),(long long)zero_us,(long long)copy_us,
              layout_cached?1:0,layout.kind==lgfx_epd_page_turn::LayoutKind::RowBlocks ? "row-blocks" :
              layout.kind==lgfx_epd_page_turn::LayoutKind::ColumnBlocks ? "column-blocks" : "diagonal-spans");
          if (animation_deferred_source) ESP_LOGI(TAG_2D,
              "page turn pipeline primed: phases=%d primed=%d slots=%d source_ready=%d/%d render=%llu us stack=%s",
              count,initial_count,animation_pipeline.slots,sources_primed,cached_phase_count,
              (unsigned long long)render_primed_us,
              animation_pipeline.external_stack ? "psram" : "internal");
          }
        } else {
          if (animation_pipeline.done) {
            vSemaphoreDelete(animation_pipeline.done); animation_pipeline.done=nullptr;
          }
          heap_caps_free(animation_frames); animation_frames=nullptr;
          if (animation_deferred_source) {
            for (int unique=0;unique<cached_phase_count;++unique)
              if (!du4_pipeline.ready[unique]) render_du4_pipeline_unique(&du4_pipeline,unique);
          }
          ESP_LOGW(TAG_2D, "page turn cache/layout unavailable: bytes=%u caps=0x%lx psram_free=%u psram_largest=%u layout_ready=%d; using normal DU4",
                   unsigned(bytes), (unsigned long)animation_cache_caps,
                   (unsigned)heap_caps_get_free_size(animation_cache_caps),
                   (unsigned)heap_caps_get_largest_free_block(animation_cache_caps),
                   int(layout_ready));
        }
      }

#if defined(CONFIG_IDF_TARGET_ESP32S31) && MYPAPER_EPD_2D_CONTINUOUS_SCAN && !MYPAPER_EPD_2D_LINE_STREAM
      // Use the FINAL physical dirty rectangle, after recovery/animation may
      // have promoted it to full screen. Configure only while panel is off.
      if (!epd2d_continuous_scan_prepare_oe_update(update.y, uint16_t(update.y + update.h))) {
        // Resource failure restores GPIO ownership; do not lose a UI update
        // solely because the experimental gate could not be allocated.
        ESP_LOGW(TAG_2D, "partial OE unavailable; using ordinary neutral-mask scan");
      }
#endif
      if (!bus->powerControl(true)) {
        ESP_LOGE(TAG_2D, "power on failed; batch aborted before scan");
        me->_physical_state_known = false;
        bus->powerControl(false);
#if defined(CONFIG_IDF_TARGET_ESP32S31)
        epd2d_continuous_scan_finish_oe_update();
#endif
        finish_animation_pipeline(&animation_pipeline,true);
        if (gc16_pipeline_active) {
          finish_gc16_phase_pipeline(&gc16_pipeline, true);
        }
        if (du4_pipeline_active) {
          finish_du4_phase_pipeline(&du4_pipeline, true);
        }
        if (phase_frames_temporary) {
          heap_caps_free(phase_frames);
        }
        heap_caps_free(phase_frame_map);
        heap_caps_free(representative_phases);
        heap_caps_free(unique_transitions);
        heap_caps_free(animation_frames);
        continue;
      }

      bool scan_ok = true;
#if (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31)) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
      uint64_t scan_total_us = 0;
      uint32_t scan_min_us = UINT32_MAX;
      uint32_t scan_max_us = 0;
      uint32_t submit_overhead_max_us = 0;
      uint64_t prefetch_total_us = 0;
      uint32_t prefetch_max_us = 0;
      const int64_t sequence_begin = esp_timer_get_time();
      for (int phase = 0; phase < sequence_phases; ++phase) {
        const uint8_t* frame_data = nullptr;
#if MYPAPER_EPD_2D_LINE_STREAM
        if (selected.type == WAVEFORM_MODE_DU) {
        {
        const uint8_t* phase_lut = selected.phases->luts + size_t(phase) * 64;
        build_transition_lut(transition, phase_lut);
        apply_gc16_transition_adjustments(
            transition, selected.type, phase, selected.phases->phases, update.changed_pixels_only);
        phase_line_context_t line_context = {
          .physical = me->_physical_buf,
          .target = me->_target_buf,
          .transition = transition,
          .panel_width = panel_w,
          .panel_height = panel_h,
          .memory_width = memory_w,
          .update_x = update.x,
          .update_y = update.y,
          .update_width = update.w,
          .update_height = update.h,
        };
        Bus_EPD::epd_2d_stream_stats_t stream_stats = {};
        const int64_t submit_begin = esp_timer_get_time();
        if (!bus->write2DStream(render_phase_line, &line_context, 100,
                                &stream_stats)) {
          ESP_LOGE(TAG_2D,
                   "phase %d/%d line stream failed: prefetch=%lu us scan=%lu us underruns=%lu",
                   phase + 1, selected.phases->phases,
                   (unsigned long)stream_stats.prefetch_time_us,
                   (unsigned long)stream_stats.scan_time_us,
                   (unsigned long)stream_stats.underruns);
          scan_ok = false;
          break;
        }
        const uint32_t submit_us = static_cast<uint32_t>(
            esp_timer_get_time() - submit_begin);
        const uint32_t scan_us = stream_stats.scan_time_us;
        const uint32_t overhead_us = submit_us > scan_us
                                   ? submit_us - scan_us : 0;
        prefetch_total_us += stream_stats.prefetch_time_us;
        prefetch_max_us = std::max(prefetch_max_us,
                                  stream_stats.prefetch_time_us);
        scan_total_us += scan_us;
        scan_min_us = std::min(scan_min_us, scan_us);
        scan_max_us = std::max(scan_max_us, scan_us);
        submit_overhead_max_us = std::max(submit_overhead_max_us, overhead_us);
        continue;
        }
        }
#endif
#if MYPAPER_EPD_2D_PRECOMPUTE_WAVEFORM
        if (animation_frames != nullptr) {
          if (animation_deferred_source &&
              __atomic_load_n(&animation_pipeline.ready[phase%animation_pipeline.slots],__ATOMIC_ACQUIRE)!=phase) {
            ESP_LOGE(TAG_2D,"page turn pipeline underrun: phase=%d; aborting without extending phase timing",phase+1);
            scan_ok=false;
            break;
          }
          frame_data = animation_frames + size_t(phase%animation_pipeline.slots)*me->_scan_frame_bytes_2d;
        } else {
        const size_t cached_phase = phase_frame_map != nullptr
                                  ? phase_frame_map[phase] : size_t(phase);
        size_t frame_slot = cached_phase;
        if (gc16_pipeline_active) {
          const int mapped_slot = __atomic_load_n(
              &gc16_pipeline.unique_slot[cached_phase], __ATOMIC_ACQUIRE);
          if (mapped_slot < 0 || mapped_slot >= gc16_pipeline.slot_count) {
            ESP_LOGE(TAG_2D,
                     "GC16 pipeline underrun: phase=%d unique=%u slot=%d ready=-1 generated=%d",
                     phase + 1, (unsigned)cached_phase, mapped_slot,
                     gc16_pipeline.next_unique);
            gc16_pipeline.failed = true;
            scan_ok = false;
            break;
          }
          frame_slot = size_t(mapped_slot);
          const int ready = __atomic_load_n(
              &gc16_pipeline.ready_unique[frame_slot], __ATOMIC_ACQUIRE);
          if (ready != int(cached_phase)) {
            ESP_LOGE(TAG_2D,
                     "GC16 pipeline underrun: phase=%d unique=%u slot=%u ready=%d generated=%d",
                     phase + 1, (unsigned)cached_phase,
                     (unsigned)frame_slot, ready,
                     gc16_pipeline.next_unique);
            gc16_pipeline.failed = true;
            scan_ok = false;
            break;
          }
        }
        if (du4_pipeline_active) {
          const bool ready = __atomic_load_n(
              &du4_pipeline.ready[cached_phase], __ATOMIC_ACQUIRE);
          if (!ready) {
            ESP_LOGE(TAG_2D,
                     "DU4 pipeline underrun: phase=%d unique=%u generated=%d",
                     phase + 1, (unsigned)cached_phase,
                     int(du4_pipeline.next_unique));
            du4_pipeline.failed = true;
            scan_ok = false;
            break;
          }
        }
        frame_data = phase_frames
                   + frame_slot * me->_scan_frame_bytes_2d;
        }
#else
        const uint8_t* phase_lut = selected.phases->luts + size_t(phase) * 64;
        build_transition_lut(transition, phase_lut);
        apply_gc16_transition_adjustments(
            transition, selected.type, phase, selected.phases->phases, update.changed_pixels_only);
        render_phase_frame(
            me->_scan_frame_2d, me->_scan_frame_bytes_2d,
            me->_physical_buf, me->_target_buf,
            panel_w, panel_h, memory_w, scan_rows,
            update.x, update.y, update.w, update.h, transition);
        frame_data = me->_scan_frame_2d;
#endif
        uint32_t scan_us = 0;
        const int64_t submit_begin = esp_timer_get_time();
        if (!bus->write2DFrame(frame_data, me->_scan_frame_bytes_2d,
                               100, &scan_us)) {
          ESP_LOGE(TAG_2D, "phase %d/%d continuous scan failed",
                   phase + 1, sequence_phases);
          scan_ok = false;
          break;
        }
        if (animation_frames && animation_deferred_source && animation_pipeline.done && phase>0) {
          // Keep a one-frame guard: write2DFrame stops/resets old GDMA at
          // its next submission, not at VSYNC return. Never reuse on VSYNC alone.
          __atomic_store_n(&animation_pipeline.ready[(phase-1)%animation_pipeline.slots],-1,__ATOMIC_RELEASE);
          if (!__atomic_load_n(&animation_pipeline.finished,__ATOMIC_ACQUIRE) && animation_pipeline.task)
            xTaskNotifyGive(animation_pipeline.task);
        }
        if (gc16_pipeline_active) {
          __atomic_store_n(&gc16_pipeline.consumed_phase, phase,
                           __ATOMIC_RELEASE);
          if (!gc16_pipeline.finished && gc16_pipeline.task != nullptr) {
            xTaskNotifyGive(gc16_pipeline.task);
          }
        }
        const uint32_t submit_us = static_cast<uint32_t>(
            esp_timer_get_time() - submit_begin);
        const uint32_t overhead_us = submit_us > scan_us
                                   ? submit_us - scan_us : 0;
        scan_total_us += scan_us;
        scan_min_us = std::min(scan_min_us, scan_us);
        scan_max_us = std::max(scan_max_us, scan_us);
        submit_overhead_max_us = std::max(submit_overhead_max_us, overhead_us);
      }
      const uint32_t sequence_us = static_cast<uint32_t>(
          esp_timer_get_time() - sequence_begin);
      if (scan_ok) {
        ESP_LOGI(TAG_2D,
                 "phase sequence complete: phases=%d sequence=%lu.%03lu ms scan_total=%llu us scan_min=%lu us scan_max=%lu us submit_overhead_max=%lu us prefetch_total=%llu us prefetch_max=%lu us backend=%s",
                 sequence_phases,
                 (unsigned long)(sequence_us / 1000),
                 (unsigned long)(sequence_us % 1000),
                 (unsigned long long)scan_total_us,
                 (unsigned long)scan_min_us,
                 (unsigned long)scan_max_us,
                 (unsigned long)submit_overhead_max_us,
                 (unsigned long long)prefetch_total_us,
                 (unsigned long)prefetch_max_us,
#if MYPAPER_EPD_2D_LINE_STREAM
                 selected.type == WAVEFORM_MODE_DU
                     ? "continuous-line-stream" : "continuous-precomputed");
#elif MYPAPER_EPD_2D_PRECOMPUTE_WAVEFORM
                 "continuous-precomputed");
#else
                 "continuous-render-before-phase");
#endif
      }
#else
      static constexpr uint8_t pair_shifts[8] = {
        4, 0, 12, 8, 20, 16, 28, 24
      };
      const size_t active_byte_begin = update.x >> 1;
      const size_t active_byte_end = (size_t(update.x) + update.w) >> 1;
      const size_t active_row_end = size_t(update.y) + update.h;

      for (int phase = 0; phase < selected.phases->phases; ++phase) {
        const uint8_t* phase_lut = selected.phases->luts + size_t(phase) * 64;
        build_transition_lut(transition, phase_lut);
        apply_gc16_transition_adjustments(
            transition, selected.type, phase, selected.phases->phases, update.changed_pixels_only);
        const int64_t phase_begin = esp_timer_get_time();
        auto render_row = [&](size_t y, uint8_t* dma, size_t dma_len) {
          memset(dma, 0, dma_len);
          uint32_t* output = reinterpret_cast<uint32_t*>(dma);

          if (y < panel_h && y >= update.y && y < active_row_end) {
            const uint8_t* from_row = me->_physical_buf + y * stride;
            const uint8_t* to_row = me->_target_buf + y * stride;
            for (size_t group = 0; group < groups_per_row; ++group) {
              uint32_t packed_output = 0;
              const size_t byte_base = group << 3;
              for (unsigned pair = 0; pair < 8; ++pair) {
                const size_t byte_index = byte_base + pair;
                if (byte_index >= active_byte_begin
                 && byte_index < active_byte_end
                 && byte_index < stride) {
                  packed_output |= uint32_t(drive_pixel_pair(
                      from_row[byte_index], to_row[byte_index], transition))
                                   << pair_shifts[pair];
                }
              }
              output[group] = permute_epd_output(packed_output);
            }
          }
        };

        uint32_t scan_begin_us = 0;
        for (size_t y = 0; y < scan_rows; ++y) {
          uint8_t* dma = me->_dma_bufs_2d[y & 1];
          render_row(y, dma, write_len);

          for (int repeat = 0; repeat < magni_h; ++repeat) {
            if (y == 0 && repeat == 0) {
              scan_begin_us = (uint32_t)esp_timer_get_time();
              bus->beginTransaction();
            } else {
              bus->scanlineDone();
            }
            bus->writeScanLine(dma, write_len);
          }
        }
        bus->scanlineDone();
        bus->endTransaction();

        const uint32_t scan_us = (uint32_t)esp_timer_get_time() - scan_begin_us;
        ESP_LOGI(TAG_2D,
                 "phase %d/%d complete: scan=%lu.%03lu ms work=%lld us",
                 phase + 1, selected.phases->phases,
                 (unsigned long)(scan_us / 1000),
                 (unsigned long)(scan_us % 1000),
                 (long long)(esp_timer_get_time() - phase_begin));
      }
#endif

      finish_animation_pipeline(&animation_pipeline,!scan_ok);
      if (animation_deferred_source && animation_frames) {
        ESP_LOGI(TAG_2D,"page turn pipeline complete: generated=%d/%d slots=%d sources=%d/%d render=%llu zero=%llu copy=%llu us",
            animation_pipeline.next,animation_pipeline.count,animation_pipeline.slots,int(du4_pipeline.next_unique),cached_phase_count,
            (unsigned long long)du4_pipeline.frame_render_us,
            (unsigned long long)animation_pipeline.zero_us,(unsigned long long)animation_pipeline.copy_us);
        if (animation_pipeline.next!=animation_pipeline.count) scan_ok=false;
      }
      if (gc16_pipeline_active) {
        finish_gc16_phase_pipeline(&gc16_pipeline, !scan_ok);
        if (scan_ok && !gc16_pipeline.failed
         && gc16_pipeline.next_unique == gc16_pipeline.unique_count) {
          ESP_LOGI(TAG_2D,
                   "GC16 pipeline complete: generated=%d/%d slots=%d lut_build=%llu us frame_render=%llu us",
                   gc16_pipeline.next_unique, gc16_pipeline.unique_count,
                   gc16_pipeline.slot_count,
                   (unsigned long long)gc16_pipeline.lut_build_us,
                   (unsigned long long)gc16_pipeline.frame_render_us);
        } else {
          ESP_LOGE(TAG_2D,
                   "GC16 pipeline incomplete: generated=%d/%d failed=%u worker=%lld ms render=%llu us",
                   gc16_pipeline.next_unique, gc16_pipeline.unique_count,
                   gc16_pipeline.failed,
                   (long long)((gc16_pipeline.producer_finished_us
                                 - gc16_pipeline.producer_started_us) / 1000),
                   (unsigned long long)gc16_pipeline.frame_render_us);
          scan_ok = false;
        }
      }
      if (du4_pipeline_active) {
        finish_du4_phase_pipeline(&du4_pipeline, !scan_ok);
        if (scan_ok && !du4_pipeline.failed
         && du4_pipeline.next_unique == du4_pipeline.unique_count) {
          ESP_LOGI(TAG_2D,
                   "DU4 pipeline complete: generated=%d/%d slots=%d frame_render=%llu us",
                   int(du4_pipeline.next_unique), du4_pipeline.unique_count,
                   du4_pipeline.unique_count,
                   (unsigned long long)du4_pipeline.frame_render_us);
        } else if (scan_ok) {
          ESP_LOGE(TAG_2D,
                   "DU4 pipeline incomplete: generated=%d/%d failed=%u",
                   int(du4_pipeline.next_unique), du4_pipeline.unique_count,
                   du4_pipeline.failed);
          scan_ok = false;
        }
      }
      if (gc16_pipeline_active || du4_pipeline_active || animation_deferred_source) {
        heap_caps_free(representative_phases);
        representative_phases = nullptr;
        heap_caps_free(unique_transitions);
        unique_transitions = nullptr;
      }

      const bool power_off_ok = bus->powerControl(false);
#if defined(CONFIG_IDF_TARGET_ESP32S31)
      epd2d_continuous_scan_finish_oe_update();
#endif
      heap_caps_free(animation_frames);
      if (phase_frames_temporary) {
        heap_caps_free(phase_frames);
      }
      heap_caps_free(phase_frame_map);
      if (!power_off_ok) {
        ESP_LOGE(TAG_2D, "power off failed; physical state invalidated");
        scan_ok = false;
      }

      if (scan_ok) {
        const size_t retained = commit_physical_rect_4bpp(
            me->_physical_buf, me->_target_buf, stride,
            update.x, update.y, update.w, update.h,
            selected.phases, selected.type);
        me->_physical_state_known = true;
        ESP_LOGI(TAG_2D,
                 "batch complete: phases=%d state=known undriven_retained=%u",
                 selected.phases->phases, (unsigned)retained);
      } else {
        me->_physical_state_known = false;
      }
      if (selected.type == WAVEFORM_MODE_GC16 || selected.type == WAVEFORM_MODE_HD
       || selected.type == WAVEFORM_MODE_DU4) {
        ESP_LOGI(TAG_2D,
                 "pair indices (after refresh): enabled=%u build=%llu us bytes=%u",
                 (unsigned)(pair_indices != nullptr),
                 (unsigned long long)pair_indices_us,
                 (unsigned)(pair_indices ? (memory_w >> 1) * scan_rows * sizeof(uint16_t) : 0));
      }
      for (int unique = 0; unique < std::min(cached_phase_count, GC16_PIPELINE_MAX_PHASES); ++unique) {
        const auto& timing = frame_timings[unique];
        if (!timing.total_us) continue;
        ESP_LOGI(TAG_2D,
                 "drive frame (after refresh): type=%u unique0=%d phase0=%d count=%u stage=%s lut=%llu clear=%llu pixels=%llu total=%llu us",
                 (unsigned)selected.type, unique, timing.phase, timing.count,
                 timing.prime ? "prime" : "worker",
                 (unsigned long long)timing.lut_us,
                 (unsigned long long)timing.clear_us,
                 (unsigned long long)timing.pixels_us,
                 (unsigned long long)timing.total_us);
      }
      if (du4_prep.valid) {
        ESP_LOGI(TAG_2D,
                 "DU4 prep summary (after refresh): stats=%llu dedup=%llu workspace=%llu active=%llu prime=%llu prime_lut=%llu prime_clear=%llu prime_pixels=%llu worker=%llu precompute=%llu us fused=%u reused=%u primed=%d slots=%d transitions=%u groups=%u/%u bytes=%u scan_ok=%u prime_fused=%u",
                 (unsigned long long)du4_prep.stats,
                 (unsigned long long)du4_prep.dedup,
                 (unsigned long long)du4_prep.workspace,
                 (unsigned long long)du4_prep.active,
                 (unsigned long long)du4_prep.prime,
                 (unsigned long long)du4_prep.prime_lut,
                 (unsigned long long)du4_prep.prime_clear,
                 (unsigned long long)du4_prep.prime_pixels,
                 (unsigned long long)du4_prep.worker,
                 (unsigned long long)du4_prep.precompute,
                 (unsigned)du4_prep.fused, (unsigned)du4_prep.reused,
                 du4_prep.primed, du4_prep.slots,
                 (unsigned)du4_prep.transitions,
                 (unsigned)du4_prep.groups, (unsigned)du4_prep.total_groups,
                 (unsigned)du4_prep.bytes, (unsigned)scan_ok, (unsigned)du4_prep.triple_prime);
      }
      ESP_LOGI(TAG_2D,
               "dispatch summary (after refresh): queue_wait=%lld snapshot=%lld us merged=%u stats_cache=%u stats_miss=%u",
               (long long)queue_wait_us, (long long)snapshot_us, merged,
               (unsigned)prepared_stats_hit, prepared_stats_miss);
    }
  }

 }
}

#endif
