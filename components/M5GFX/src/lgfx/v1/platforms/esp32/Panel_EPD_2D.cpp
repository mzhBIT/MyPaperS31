#include "Panel_EPD_2D.hpp"

#if SOC_LCD_I80_SUPPORTED && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4))

#include "lgfx/v1/platforms/common.hpp"
#include "lgfx/v1/platforms/esp32/MyPaper_EPD_Config.hpp"

#include <algorithm>
#include <cstring>
#include <climits>

#include <esp_heap_caps.h>
#include <esp_log.h>
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

  static void cache_writeback_2d(const void* ptr, size_t size)
  {
#if defined(ESP_CACHE_MSYNC_FLAG_DIR_C2M)
    if (ptr != nullptr && size != 0 && !isEmbeddedMemory(ptr)) {
      uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~uintptr_t(127);
      uintptr_t end = (reinterpret_cast<uintptr_t>(ptr) + size + 127) & ~uintptr_t(127);
      if (start < end) {
        esp_cache_msync(reinterpret_cast<void*>(start), end - start,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
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

  static size_t collect_used_transitions_4bpp(
      bool* used, const uint8_t* physical, const uint8_t* target,
      size_t stride, size_t panel_height,
      uint16_t x, uint16_t y, uint16_t width, uint16_t height)
  {
    memset(used, 0, 256 * sizeof(*used));
    const size_t byte_begin = std::min<size_t>(x >> 1, stride);
    const size_t byte_end = std::min<size_t>(
        (size_t(x) + width + 1) >> 1, stride);
    const size_t row_begin = std::min<size_t>(y, panel_height);
    const size_t row_end = std::min<size_t>(size_t(y) + height, panel_height);
    size_t count = 0;
    for (size_t row = row_begin; row < row_end; ++row) {
      const uint8_t* from_row = physical + row * stride;
      const uint8_t* to_row = target + row * stride;
      for (size_t index = byte_begin; index < byte_end; ++index) {
        const uint8_t from = from_row[index];
        const uint8_t to = to_row[index];
        const uint8_t high = (from & 0xF0) | (to >> 4);
        const uint8_t low = ((from & 0x0F) << 4) | (to & 0x0F);
        if (!used[high]) { used[high] = true; ++count; }
        if (!used[low]) { used[low] = true; ++count; }
      }
    }
    return count;
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

  static void apply_gc16_black_hold_compensation(
      uint8_t* transition, uint8_t waveform_type,
      int phase, int phase_count)
  {
#if MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES > 0
    if (waveform_type != WAVEFORM_MODE_GC16 || phase_count < 2) {
      return;
    }
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
#else
    (void)transition;
    (void)waveform_type;
    (void)phase;
    (void)phase_count;
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
      apply_gc16_black_hold_compensation(
          transition, waveform_type, phase, phases->phases);
      for (size_t i = 0; i < 256; ++i) {
        driven[i] |= transition[i] != 0;
      }
    }
  }

  static size_t collect_active_groups_fullscreen(
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

  // GC16 Quality always scans the complete panel.  A 64 KiB pair LUT maps
  // the two old 4bpp pixels and two target 4bpp pixels directly to their two
  // 2-bit drive values, removing the hot loop's masks and dual transition
  // lookups without changing a single waveform output bit.
  __attribute__((optimize("O3"))) static void render_gc16_pair_phase_frame(
      uint8_t* output_frame,
      const uint8_t* physical, const uint8_t* target,
      size_t memory_width, size_t scan_rows,
      const uint8_t* pair_lut)
  {
    static constexpr uint8_t pair_shifts[8] = {
      4, 0, 12, 8, 20, 16, 28, 24
    };
    const size_t stride = memory_width >> 1;
    const size_t groups_per_row = memory_width >> 4;
    const size_t output_stride = memory_width >> 2;

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
        output[group] = permute_epd_output(packed);
      }
    }
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
    uint8_t* frames = nullptr;
    size_t frame_bytes = 0;
    const uint8_t* physical = nullptr;
    const uint8_t* target = nullptr;
    uint8_t* pair_lut = nullptr;
    const uint8_t* unique_transitions = nullptr;
    size_t memory_width = 0;
    size_t scan_rows = 0;
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
  };

  static void render_gc16_pipeline_unique(gc16_phase_pipeline_t* ctx,
                                          int unique, int slot)
  {
    const uint8_t* transition =
        ctx->unique_transitions + size_t(unique) * 256;
    const int64_t lut_begin = esp_timer_get_time();
    build_gc16_pair_lut(ctx->pair_lut, transition);
    ctx->lut_build_us += esp_timer_get_time() - lut_begin;
    const int64_t render_begin = esp_timer_get_time();
    render_gc16_pair_phase_frame(
        ctx->frames + size_t(slot) * ctx->frame_bytes,
        ctx->physical, ctx->target,
        ctx->memory_width, ctx->scan_rows, ctx->pair_lut);
    ctx->frame_render_us += esp_timer_get_time() - render_begin;
    __atomic_store_n(&ctx->unique_slot[unique], slot, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->ready_unique[slot], unique, __ATOMIC_RELEASE);
  }

  static void gc16_phase_producer_task(void* argument)
  {
    auto* ctx = static_cast<gc16_phase_pipeline_t*>(argument);
    for (int unique = ctx->next_unique;
         unique < ctx->unique_count && !ctx->abort; ++unique) {
      int slot = -1;
      while (!ctx->abort && slot < 0) {
        const int consumed = __atomic_load_n(
            &ctx->consumed_phase, __ATOMIC_ACQUIRE);
        for (int candidate = 0; candidate < ctx->slot_count; ++candidate) {
          const int resident = __atomic_load_n(
              &ctx->ready_unique[candidate], __ATOMIC_ACQUIRE);
          if (resident < 0 || consumed >= ctx->last_phase[resident]) {
            slot = candidate;
            if (resident >= 0) {
              __atomic_store_n(&ctx->unique_slot[resident], -1,
                               __ATOMIC_RELEASE);
            }
            __atomic_store_n(&ctx->ready_unique[candidate], -1,
                             __ATOMIC_RELEASE);
            break;
          }
        }
        if (slot < 0) {
          ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
      }
      if (ctx->abort) {
        break;
      }
      render_gc16_pipeline_unique(ctx, unique, slot);
      ctx->next_unique = unique + 1;
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
      const uint16_t* active_groups, size_t active_group_count)
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

    if (full_screen && active_groups != nullptr) {
      memset(output_frame, 0, output_bytes);
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
        output[group_index] = permute_epd_output(packed);
      }
      return;
    }

    if (!full_screen) {
      memset(output_frame, 0, output_bytes);
    }
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
        output[group] = permute_epd_output(packed);
      }
    }
  }

  static constexpr int DU4_PIPELINE_MAX_PHASES = 64;

  struct du4_phase_pipeline_t
  {
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
    render_du4_phase_frame(
        ctx->frames + size_t(unique) * ctx->frame_bytes,
        ctx->frame_bytes, ctx->physical, ctx->target,
        ctx->panel_width, ctx->panel_height,
        ctx->memory_width, ctx->scan_rows,
        ctx->update_x, ctx->update_y,
        ctx->update_width, ctx->update_height,
        ctx->pair_lut, ctx->pair_lut + DU4_PAIR_LUT_BYTES,
        du4_equal_pairs_are_idle(transition),
        ctx->active_groups, ctx->active_group_count);
    ctx->frame_render_us += esp_timer_get_time() - render_begin;
    __atomic_store_n(&ctx->ready[unique], true, __ATOMIC_RELEASE);
  }

  static void du4_phase_producer_task(void* argument)
  {
    auto* ctx = static_cast<du4_phase_pipeline_t*>(argument);
    for (int unique = ctx->next_unique;
         unique < ctx->unique_count && !ctx->abort; ++unique) {
      render_du4_pipeline_unique(ctx, unique);
      __atomic_store_n(&ctx->next_unique, unique + 1, __ATOMIC_RELEASE);
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
                                      ? quantize_a2_gray(new_high) : new_high);
        const uint8_t physical_low = waveform_type == WAVEFORM_MODE_DU4
                                   ? quantize_du4_gray(new_low)
                                   : (waveform_type == WAVEFORM_MODE_A2
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
#if defined(CONFIG_IDF_TARGET_ESP32P4) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
    _scan_frame_bytes_2d = (memory_w >> 2) * memory_h;
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
#if defined(CONFIG_IDF_TARGET_ESP32P4) \
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
#if defined(CONFIG_IDF_TARGET_ESP32P4) \
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

  void Panel_EPD_2D::display(uint_fast16_t x, uint_fast16_t y,
                             uint_fast16_t w, uint_fast16_t h)
  {
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
      ESP_LOGW(TAG_2D, "display skipped: update range is empty");
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
    };

    const size_t stride = size_t(_cfg.panel_width) >> 1;
    cache_writeback_2d(_buf + size_t(_range_mod.top) * stride,
                       size_t(update.h) * stride);
    _display_busy_2d = true;
    const bool queued = xQueueSend(_update_queue_2d, &update,
                                   128 / portTICK_PERIOD_MS) == pdTRUE;
    if (queued) {
      ESP_LOGI(TAG_2D, "refresh queued: x=%u y=%u w=%u h=%u mode=%u",
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
      me->_display_busy_2d = true;

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

      if (!me->_physical_state_known) {
        if (update.mode != epd_mode_t::epd_quality) {
          ESP_LOGW(TAG_2D, "physical state unknown; promoting first update to Quality");
        }
        update.mode = epd_mode_t::epd_quality;
      }
      if (update.mode == epd_mode_t::epd_quality) {
        // The supplied GC16 is an official full-refresh waveform.  Keep its
        // old/new state coherent by always treating Quality as one full batch.
        update.x = 0;
        update.y = 0;
        update.w = panel_w;
        update.h = panel_h;
      }

      memcpy(me->_target_buf, me->_buf, frame_bytes);
      const selected_waveform_t selected = me->selectWaveform(update.mode);
      if (selected.phases == nullptr || selected.phases->luts == nullptr
       || selected.phases->phases <= 0) {
        ESP_LOGE(TAG_2D, "no waveform for display mode %u", (unsigned)update.mode);
        me->_physical_state_known = false;
        continue;
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
      if (selected.type == WAVEFORM_MODE_GC16) {
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
      bool du4_pipeline_active = false;
      du4_phase_pipeline_t du4_pipeline = {};
#if defined(CONFIG_IDF_TARGET_ESP32P4) \
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

#if MYPAPER_EPD_2D_DU_PHASE_DEDUP
        if (selected.type == WAVEFORM_MODE_GC16
         || selected.type == WAVEFORM_MODE_DU
         || selected.type == WAVEFORM_MODE_A2
         || selected.type == WAVEFORM_MODE_DU4) {
          phase_frame_map = static_cast<uint16_t*>(heap_caps_malloc(
              size_t(phase_count) * sizeof(uint16_t), MALLOC_CAP_8BIT));
          representative_phases = static_cast<uint16_t*>(heap_caps_malloc(
              size_t(phase_count) * sizeof(uint16_t), MALLOC_CAP_8BIT));
          unique_transitions = static_cast<uint8_t*>(heap_caps_malloc(
              size_t(phase_count) * 256, MALLOC_CAP_8BIT));
          if (phase_frame_map != nullptr && representative_phases != nullptr
           && unique_transitions != nullptr) {
            bool used_transitions[256];
            used_transition_count = collect_used_transitions_4bpp(
                used_transitions, me->_physical_buf, me->_target_buf,
                stride, panel_h,
                update.x, update.y, update.w, update.h);
            cached_phase_count = 0;
            for (int phase = 0; phase < phase_count; ++phase) {
              const uint8_t* phase_lut = selected.phases->luts
                                       + size_t(phase) * 64;
              build_transition_lut(transition, phase_lut);
              apply_gc16_black_hold_compensation(
                  transition, selected.type, phase, phase_count);

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

        const bool gc16_pair_renderer =
#if MYPAPER_EPD_2D_GC16_PAIR_LUT
            selected.type == WAVEFORM_MODE_GC16
         && me->_gc16_pair_lut_2d != nullptr
         && update.x == 0 && update.y == 0
         && update.w == panel_w && update.h == panel_h
         && panel_w == memory_w && scan_rows == panel_h;
#else
            false;
#endif
        const bool gc16_pipeline_requested =
#if MYPAPER_EPD_2D_GC16_PIPELINE
            gc16_pair_renderer && deduplicated
         && cached_phase_count > MYPAPER_EPD_2D_GC16_PIPELINE_PRIME
         && cached_phase_count <= GC16_PIPELINE_MAX_PHASES
         && MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS
                <= GC16_PIPELINE_MAX_SLOTS
         && MYPAPER_EPD_2D_GC16_PIPELINE_PRIME > 0
         && MYPAPER_EPD_2D_GC16_PIPELINE_PRIME
                < MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS
         && uxTaskPriorityGet(nullptr) > 1;
#else
            false;
#endif
        const bool du4_pipeline_requested =
#if MYPAPER_EPD_2D_DU4_PIPELINE
            selected.type == WAVEFORM_MODE_DU4
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
        if (selected.type == WAVEFORM_MODE_DU4) {
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
                   "phase cache allocation failed: phases=%d cached=%d bytes=%u; batch aborted to preserve waveform timing",
                   phase_count, cached_phase_count,
                   (unsigned)phase_cache_bytes);
          heap_caps_free(phase_frame_map);
          heap_caps_free(representative_phases);
          heap_caps_free(unique_transitions);
          me->_physical_state_known = false;
          continue;
        }

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
          gc16_pipeline.unique_count = cached_phase_count;
          gc16_pipeline.slot_count = phase_storage_count;
          gc16_pipeline.next_unique = std::min<int>(
              MYPAPER_EPD_2D_GC16_PIPELINE_PRIME, cached_phase_count);
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
        uint64_t pair_lut_build_us = 0;
        uint64_t phase_render_us = 0;
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
          bool driven_transitions[256];
          build_driven_transition_mask(
              driven_transitions, selected.phases, selected.type);
          du4_active_group_count = collect_active_groups_fullscreen(
              me->_du4_active_groups_2d,
              me->_du4_active_group_capacity_2d,
              me->_physical_buf, me->_target_buf,
              stride, panel_h, driven_transitions);
          // Clearing a complete frame is cheap, but sparse indexing stops
          // paying off when almost every 16-pixel group still needs work.
          if (du4_active_group_count * 8u < total_group_count * 7u) {
            du4_active_groups = me->_du4_active_groups_2d;
          }
        }
        if (du4_pipeline_requested) {
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
          du4_pipeline.done = xSemaphoreCreateBinary();
          if (du4_pipeline.done == nullptr) {
            ESP_LOGW(TAG_2D,
                     "DU4 pipeline semaphore allocation failed; using full precompute");
          } else {
            du4_pipeline_active = true;
          }
        }
        const int initial_render_count = gc16_pipeline_active
            ? gc16_pipeline.next_unique
            : du4_pipeline_active ? int(du4_pipeline.next_unique)
                                  : cached_phase_count;
        for (int cached = 0; cached < initial_render_count; ++cached) {
          const int phase = deduplicated ? representative_phases[cached]
                                         : cached;
          const uint8_t* render_transition = nullptr;
          if (deduplicated) {
            render_transition = unique_transitions + size_t(cached) * 256;
          } else {
            const uint8_t* phase_lut = selected.phases->luts
                                     + size_t(phase) * 64;
            build_transition_lut(transition, phase_lut);
            apply_gc16_black_hold_compensation(
                transition, selected.type, phase, phase_count);
            render_transition = transition;
          }
          if (gc16_pair_renderer) {
            const int64_t lut_begin = esp_timer_get_time();
            build_gc16_pair_lut(
                me->_gc16_pair_lut_2d, render_transition);
            pair_lut_build_us += esp_timer_get_time() - lut_begin;
            const int64_t render_begin = esp_timer_get_time();
            render_gc16_pair_phase_frame(
                phase_frames + size_t(cached) * phase_frame_bytes,
                me->_physical_buf, me->_target_buf,
                memory_w, scan_rows, me->_gc16_pair_lut_2d);
            phase_render_us += esp_timer_get_time() - render_begin;
          } else if (selected.type == WAVEFORM_MODE_DU4
           && me->_du4_pair_lut_2d != nullptr) {
            const int64_t render_begin = esp_timer_get_time();
            build_du4_pair_lut(me->_du4_pair_lut_2d, render_transition);
            render_du4_phase_frame(
                phase_frames + size_t(cached) * phase_frame_bytes,
                phase_frame_bytes, me->_physical_buf, me->_target_buf,
                panel_w, panel_h, memory_w, scan_rows,
                update.x, update.y, update.w, update.h,
                me->_du4_pair_lut_2d,
                me->_du4_pair_lut_2d + DU4_PAIR_LUT_BYTES,
                du4_equal_pairs_are_idle(render_transition),
                du4_active_groups, du4_active_group_count);
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
                   "waveform pipeline primed before power-on: phases=%d cached=%d transitions=%u slots=%d primed=%d bytes=%u time=%lu.%03lu ms lut_build=%llu us frame_render=%llu us renderer=gc16-pair-lut-ring stack=%s",
                   phase_count, cached_phase_count,
                   (unsigned)used_transition_count, phase_storage_count,
                   initial_render_count, (unsigned)phase_cache_bytes,
                   (unsigned long)(precompute_us / 1000),
                   (unsigned long)(precompute_us % 1000),
                   (unsigned long long)pair_lut_build_us,
                   (unsigned long long)phase_render_us,
                   gc16_pipeline.external_stack ? "psram" : "internal");
        } else if (du4_pipeline_active) {
          du4_pipeline.frame_render_us = phase_render_us;
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
          ESP_LOGI(TAG_2D,
                   "waveform pipeline primed before power-on: phases=%d cached=%d transitions=%u slots=%d primed=%d bytes=%u time=%lu.%03lu ms frame_render=%llu us renderer=du4-pair-lut-deferred workspace=%s active_groups=%u/%u path=%s stack=%s",
                   phase_count, cached_phase_count,
                   (unsigned)used_transition_count, phase_storage_count,
                   initial_render_count, (unsigned)phase_cache_bytes,
                   (unsigned long)(precompute_us / 1000),
                   (unsigned long)(precompute_us % 1000),
                   (unsigned long long)phase_render_us,
                   du4_workspace_reused ? "reused" : "grown",
                   (unsigned)du4_active_group_count,
                   (unsigned)total_group_count,
                   du4_active_groups != nullptr ? "sparse" : "full",
                   du4_pipeline.external_stack ? "psram" : "internal");
        } else {
          ESP_LOGI(TAG_2D,
                   "waveform prepared before power-on: phases=%d cached=%d transitions=%u bytes=%u time=%lu.%03lu ms lut_build=%llu us frame_render=%llu us renderer=%s workspace=%s active_groups=%u/%u path=%s",
                   phase_count, cached_phase_count,
                   (unsigned)used_transition_count, (unsigned)phase_cache_bytes,
                   (unsigned long)(precompute_us / 1000),
                   (unsigned long)(precompute_us % 1000),
                   (unsigned long long)pair_lut_build_us,
                   (unsigned long long)phase_render_us,
                   gc16_pair_renderer ? "gc16-pair-lut"
                     : selected.type == WAVEFORM_MODE_DU4
                     && me->_du4_pair_lut_2d != nullptr
                       ? "du4-pair-lut" : "generic",
                   selected.type == WAVEFORM_MODE_DU4
                       ? (du4_workspace_reused ? "reused" : "grown")
                       : "temporary",
                   (unsigned)du4_active_group_count,
                   (unsigned)total_group_count,
                   du4_active_groups != nullptr ? "sparse" : "full");
          heap_caps_free(representative_phases);
          representative_phases = nullptr;
          heap_caps_free(unique_transitions);
          unique_transitions = nullptr;
        }
      }
#endif

      if (!bus->powerControl(true)) {
        ESP_LOGE(TAG_2D, "power on failed; batch aborted before scan");
        me->_physical_state_known = false;
        bus->powerControl(false);
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
        continue;
      }

      bool scan_ok = true;
#if defined(CONFIG_IDF_TARGET_ESP32P4) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN
      uint64_t scan_total_us = 0;
      uint32_t scan_min_us = UINT32_MAX;
      uint32_t scan_max_us = 0;
      uint32_t submit_overhead_max_us = 0;
      uint64_t prefetch_total_us = 0;
      uint32_t prefetch_max_us = 0;
      const int64_t sequence_begin = esp_timer_get_time();
      for (int phase = 0; phase < selected.phases->phases; ++phase) {
        const uint8_t* frame_data = nullptr;
#if MYPAPER_EPD_2D_LINE_STREAM
        if (selected.type == WAVEFORM_MODE_DU) {
        {
        const uint8_t* phase_lut = selected.phases->luts + size_t(phase) * 64;
        build_transition_lut(transition, phase_lut);
        apply_gc16_black_hold_compensation(
            transition, selected.type, phase, selected.phases->phases);
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
#else
        const uint8_t* phase_lut = selected.phases->luts + size_t(phase) * 64;
        build_transition_lut(transition, phase_lut);
        apply_gc16_black_hold_compensation(
            transition, selected.type, phase, selected.phases->phases);
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
                   phase + 1, selected.phases->phases);
          scan_ok = false;
          break;
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
                 selected.phases->phases,
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
        apply_gc16_black_hold_compensation(
            transition, selected.type, phase, selected.phases->phases);
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
        } else if (scan_ok) {
          ESP_LOGE(TAG_2D,
                   "GC16 pipeline incomplete: generated=%d/%d failed=%u",
                   gc16_pipeline.next_unique, gc16_pipeline.unique_count,
                   gc16_pipeline.failed);
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
      if (gc16_pipeline_active || du4_pipeline_active) {
        heap_caps_free(representative_phases);
        representative_phases = nullptr;
        heap_caps_free(unique_transitions);
        unique_transitions = nullptr;
      }

      const bool power_off_ok = bus->powerControl(false);
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
    }
  }

 }
}

#endif
