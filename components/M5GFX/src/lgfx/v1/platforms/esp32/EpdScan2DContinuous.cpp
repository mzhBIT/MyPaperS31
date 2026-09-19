#include "EpdScan2DContinuous.hpp"
#include "MyPaper_EPD_Config.hpp"

#if defined(CONFIG_IDF_TARGET_ESP32P4) \
 && MYPAPER_EPD_USE_2D_ENGINE && MYPAPER_EPD_2D_CONTINUOUS_SCAN

#include <algorithm>
#include <cstring>

#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_private/esp_dma_utils.h>
#include <esp_private/gdma.h>
#include <esp_private/gdma_link.h>
#include <esp_private/gpio.h>
#include <esp_private/periph_ctrl.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <hal/dma_types.h>
#include <hal/lcd_hal.h>
#include <hal/lcd_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/interrupts.h>
#include <soc/io_mux_reg.h>
#include <soc/lcd_cam_struct.h>
#include <soc/lcd_periph.h>
#include <soc/soc_caps.h>

// ESP32-P4's public esp_lcd RGB driver is disabled in IDF 5.3 even though the
// LCD_CAM RGB timing registers and signals are present.  This backend uses the
// public GDMA/RMT drivers and only the LCD HAL/LL register helpers.

namespace lgfx
{
 inline namespace v1
 {

  namespace
  {
    static constexpr char TAG[] = "EPD_2D_SCAN";
    static constexpr uint32_t RMT_RESOLUTION_HZ = 10000000u; // 0.1 us
    static constexpr size_t DMA_NODE_MAX_BYTES = DMA_DESCRIPTOR_BUFFER_MAX_SIZE;
    static constexpr size_t STREAM_BOUNCE_LINES = 4;
    static constexpr size_t STREAM_QUEUE_LINES = 96;
    static constexpr size_t STREAM_PREFETCH_LINES = STREAM_QUEUE_LINES * 2 - 8;

    struct continuous_scan_state_t
    {
      lcd_hal_context_t lcd_hal = {};
      gdma_channel_handle_t dma_channel = nullptr;
      gdma_link_list_handle_t dma_links = nullptr;
      gdma_link_list_handle_t stream_dma_links = nullptr;
      intr_handle_t lcd_interrupt = nullptr;
      rmt_channel_handle_t ckv_channel = nullptr;
      rmt_encoder_handle_t ckv_encoder = nullptr;
      SemaphoreHandle_t frame_done = nullptr;
      SemaphoreHandle_t producer_done[2] = {};
      SemaphoreHandle_t producer_space[2] = {};
      TaskHandle_t producer_tasks[2] = {};
      Bus_EPD::config_t bus = {};
      size_t active_words = 0;
      size_t rows = 0;
      size_t frame_bytes = 0;
      uint32_t line_period_us = MYPAPER_EPD_2D_LINE_PERIOD_US;
      size_t dma_alignment = 64;
      size_t line_bytes = 0;
      size_t bounce_bytes = 0;
      uint8_t* bounce_buffers[2] = {};
      uint8_t* line_queues[2] = {};
      volatile uint32_t queue_read[2] = {};
      volatile uint32_t queue_write[2] = {};
      volatile uint32_t lines_prepared = 0;
      volatile uint32_t lines_consumed = 0;
      volatile uint32_t bounce_eof_count = 0;
      volatile uint32_t stream_underruns = 0;
      volatile bool stream_active = false;
      volatile bool producer_abort = false;
      Bus_EPD::epd_2d_line_renderer_t line_renderer = nullptr;
      void* line_context = nullptr;
      bool initialized = false;
    };

    continuous_scan_state_t scan_state;

    static void IRAM_ATTR stream_fill_bounce(continuous_scan_state_t* state,
                                             uint8_t* output)
    {
      for (size_t i = 0; i < STREAM_BOUNCE_LINES; ++i) {
        uint8_t* dst = output + i * state->line_bytes;
        const uint32_t row = __atomic_fetch_add(&state->lines_consumed, 1,
                                                 __ATOMIC_RELAXED);
        if (row >= state->rows) {
          memset(dst, 0, state->line_bytes);
          continue;
        }
        const unsigned producer = row & 1u;
        const uint32_t read = __atomic_load_n(&state->queue_read[producer],
                                               __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&state->queue_write[producer],
                                                __ATOMIC_ACQUIRE);
        if (read == write) {
          memset(dst, 0, state->line_bytes);
          __atomic_fetch_add(&state->stream_underruns, 1, __ATOMIC_RELAXED);
          continue;
        }
        memcpy(dst,
               state->line_queues[producer]
                   + (read % STREAM_QUEUE_LINES) * state->line_bytes,
               state->line_bytes);
        __atomic_store_n(&state->queue_read[producer], read + 1,
                         __ATOMIC_RELEASE);
      }
    }

    static bool IRAM_ATTR stream_dma_eof_callback(
        gdma_channel_handle_t, gdma_event_data_t*, void* argument)
    {
      auto* state = static_cast<continuous_scan_state_t*>(argument);
      if (!state->stream_active) {
        return false;
      }
      const uint32_t index = __atomic_fetch_add(&state->bounce_eof_count, 1,
                                                __ATOMIC_RELAXED) & 1u;
      stream_fill_bounce(state, state->bounce_buffers[index]);
      // Every four-line bounce consumes two rows from each producer queue.
      // Wake producers that blocked when their queue became full.  A binary
      // semaphore is sufficient: the producer re-checks the actual counters
      // and fills all space that is available before blocking again.
      BaseType_t wake = pdFALSE;
      xSemaphoreGiveFromISR(state->producer_space[0], &wake);
      xSemaphoreGiveFromISR(state->producer_space[1], &wake);
      return wake == pdTRUE;
    }

    static void stream_producer_task(void* argument)
    {
      const unsigned producer = static_cast<unsigned>(
          reinterpret_cast<uintptr_t>(argument));
      auto* state = &scan_state;
      for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (uint32_t row = producer; row < state->rows; row += 2) {
          if (state->producer_abort) {
            break;
          }
          uint32_t write;
          for (;;) {
            write = __atomic_load_n(&state->queue_write[producer],
                                    __ATOMIC_RELAXED);
            const uint32_t read = __atomic_load_n(&state->queue_read[producer],
                                                   __ATOMIC_ACQUIRE);
            if (write - read < STREAM_QUEUE_LINES - 1) {
              break;
            }
            // taskYIELD() only schedules an equal-priority task.  These
            // producers run above the caller, so spinning here starves both
            // the caller and the per-core Idle task and eventually trips the
            // task watchdog.  Sleep until DMA consumption creates space.
            xSemaphoreTake(state->producer_space[producer], portMAX_DELAY);
            if (state->producer_abort) {
              break;
            }
          }
          if (state->producer_abort) {
            break;
          }
          uint8_t* dst = state->line_queues[producer]
                       + (write % STREAM_QUEUE_LINES) * state->line_bytes;
          state->line_renderer(state->line_context, row, dst,
                               state->line_bytes);
          __atomic_store_n(&state->queue_write[producer], write + 1,
                           __ATOMIC_RELEASE);
          __atomic_fetch_add(&state->lines_prepared, 1, __ATOMIC_RELAXED);
        }
        xSemaphoreGive(state->producer_done[producer]);
      }
    }

    static void stop_stream_producers(continuous_scan_state_t* state)
    {
      state->producer_abort = true;
      for (unsigned i = 0; i < 2; ++i) {
        __atomic_store_n(&state->queue_read[i],
                         __atomic_load_n(&state->queue_write[i],
                                         __ATOMIC_RELAXED),
                         __ATOMIC_RELEASE);
        // Release a producer that is blocked on a full queue.
        xSemaphoreGive(state->producer_space[i]);
      }
      for (unsigned i = 0; i < 2; ++i) {
        xSemaphoreTake(state->producer_done[i], portMAX_DELAY);
      }
    }

    static void connect_lcd_signal(int gpio, int signal)
    {
      const auto gpio_num = static_cast<gpio_num_t>(gpio);
      gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
      gpio_func_sel(gpio_num, PIN_FUNC_GPIO);
      esp_rom_gpio_connect_out_signal(gpio, signal, false, false);
    }

    static void IRAM_ATTR lcd_frame_isr(void* argument)
    {
      auto* state = static_cast<continuous_scan_state_t*>(argument);
      const uint32_t status = lcd_ll_get_interrupt_status(state->lcd_hal.dev);
      lcd_ll_clear_interrupt_status(state->lcd_hal.dev, status);
      if ((status & LCD_LL_EVENT_VSYNC_END) == 0) {
        return;
      }

      lcd_ll_stop(state->lcd_hal.dev);
      BaseType_t wake = pdFALSE;
      xSemaphoreGiveFromISR(state->frame_done, &wake);
      if (wake == pdTRUE) {
        portYIELD_FROM_ISR();
      }
    }

    static bool init_dma(continuous_scan_state_t* state)
    {
      gdma_channel_alloc_config_t channel_config = {};
      channel_config.direction = GDMA_CHANNEL_DIRECTION_TX;
      esp_err_t err = gdma_new_axi_channel(&channel_config, &state->dma_channel);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "gdma_new_axi_channel failed: %s", esp_err_to_name(err));
        return false;
      }
      err = gdma_connect(state->dma_channel,
                         GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "gdma_connect failed: %s", esp_err_to_name(err));
        return false;
      }

      gdma_strategy_config_t strategy = {};
      strategy.auto_update_desc = true;
      strategy.owner_check = true;
      if ((err = gdma_apply_strategy(state->dma_channel, &strategy)) != ESP_OK) {
        ESP_LOGE(TAG, "gdma_apply_strategy failed: %s", esp_err_to_name(err));
        return false;
      }

      gdma_transfer_config_t transfer = {};
      transfer.max_data_burst_size = 64;
      transfer.access_ext_mem = true;
      if ((err = gdma_config_transfer(state->dma_channel, &transfer)) != ESP_OK) {
        ESP_LOGE(TAG, "gdma_config_transfer failed: %s", esp_err_to_name(err));
        return false;
      }

      size_t internal_alignment = 0;
      size_t external_alignment = 0;
      if ((err = gdma_get_alignment_constraints(state->dma_channel,
                                                 &internal_alignment,
                                                 &external_alignment)) != ESP_OK) {
        ESP_LOGE(TAG, "gdma alignment query failed: %s", esp_err_to_name(err));
        return false;
      }
      state->dma_alignment = std::max<size_t>(64,
          std::max(internal_alignment, external_alignment));
      const size_t node_count = esp_dma_calculate_node_count(
          state->frame_bytes, state->dma_alignment, DMA_NODE_MAX_BYTES);
      gdma_link_list_config_t links = {};
      links.num_items = node_count;
      links.item_alignment = 8; // AXI GDMA descriptor alignment on ESP32-P4.
      links.flags.check_owner = true;
      if ((err = gdma_new_link_list(&links, &state->dma_links)) != ESP_OK) {
        ESP_LOGE(TAG, "gdma_new_link_list(%u) failed: %s",
                 (unsigned)node_count, esp_err_to_name(err));
        return false;
      }

      gdma_tx_event_callbacks_t callbacks = {};
      callbacks.on_trans_eof = stream_dma_eof_callback;
      if ((err = gdma_register_tx_event_callbacks(state->dma_channel,
                                                   &callbacks, state)) != ESP_OK) {
        ESP_LOGE(TAG, "stream DMA callback registration failed: %s",
                 esp_err_to_name(err));
        return false;
      }
      return true;
    }

    static bool init_streaming(continuous_scan_state_t* state)
    {
      state->line_bytes = state->active_words * sizeof(uint16_t);
      state->bounce_bytes = state->line_bytes * STREAM_BOUNCE_LINES;
      for (unsigned i = 0; i < 2; ++i) {
        state->bounce_buffers[i] = static_cast<uint8_t*>(
            heap_caps_aligned_calloc(state->dma_alignment, 1,
                                     state->bounce_bytes,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        state->line_queues[i] = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16,
                                    STREAM_QUEUE_LINES * state->line_bytes,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        state->producer_done[i] = xSemaphoreCreateBinary();
        state->producer_space[i] = xSemaphoreCreateBinary();
        if (state->bounce_buffers[i] == nullptr
         || state->line_queues[i] == nullptr
         || state->producer_done[i] == nullptr
         || state->producer_space[i] == nullptr) {
          ESP_LOGE(TAG, "line stream allocation failed: bounce=%p queue=%p done=%p space=%p",
                   state->bounce_buffers[i], state->line_queues[i],
                   state->producer_done[i], state->producer_space[i]);
          return false;
        }
      }

      const size_t nodes_per_buffer = esp_dma_calculate_node_count(
          state->bounce_bytes, state->dma_alignment, DMA_NODE_MAX_BYTES);
      gdma_link_list_config_t links = {};
      links.num_items = nodes_per_buffer * 2;
      links.item_alignment = 8;
      links.flags.check_owner = true;
      esp_err_t err = gdma_new_link_list(&links, &state->stream_dma_links);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream link allocation failed: %s", esp_err_to_name(err));
        return false;
      }
      gdma_buffer_mount_config_t mounts[2] = {};
      for (unsigned i = 0; i < 2; ++i) {
        mounts[i].buffer = state->bounce_buffers[i];
        mounts[i].buffer_alignment = state->dma_alignment;
        mounts[i].length = state->bounce_bytes;
        mounts[i].flags.mark_eof = true;
      }
      if ((err = gdma_link_mount_buffers(state->stream_dma_links, 0, mounts,
                                         2, nullptr)) != ESP_OK) {
        ESP_LOGE(TAG, "stream bounce mount failed: %s", esp_err_to_name(err));
        return false;
      }

      for (unsigned i = 0; i < 2; ++i) {
        if (xTaskCreatePinnedToCore(stream_producer_task, "epd2d_line", 3072,
                                    reinterpret_cast<void*>(uintptr_t(i)),
                                    configMAX_PRIORITIES - 2,
                                    &state->producer_tasks[i], i) != pdPASS) {
          ESP_LOGE(TAG, "line producer %u creation failed", i);
          return false;
        }
      }
      ESP_LOGI(TAG,
               "line stream ready: producers=2 queue=%u lines/core bounce=%u lines buffer=%u bytes",
               (unsigned)STREAM_QUEUE_LINES, (unsigned)STREAM_BOUNCE_LINES,
               (unsigned)state->bounce_bytes);
      return true;
    }

    static bool init_ckv(continuous_scan_state_t* state)
    {
      rmt_tx_channel_config_t channel = {};
      channel.gpio_num = static_cast<gpio_num_t>(state->bus.pin_ckv);
      channel.clk_src = RMT_CLK_SRC_DEFAULT;
      channel.resolution_hz = RMT_RESOLUTION_HZ;
      channel.mem_block_symbols = 64;
      channel.trans_queue_depth = 1;
      channel.flags.init_level = 0;
      esp_err_t err = rmt_new_tx_channel(&channel, &state->ckv_channel);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return false;
      }
      rmt_copy_encoder_config_t encoder_config = {};
      if ((err = rmt_new_copy_encoder(&encoder_config, &state->ckv_encoder)) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        return false;
      }
      if ((err = rmt_enable(state->ckv_channel)) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return false;
      }
      return true;
    }

    static bool init_lcd(continuous_scan_state_t* state)
    {
      // ESP32-P4 no longer implements periph_module_enable/reset.  Acquire the
      // LCD_CAM RCC reference exactly as IDF's I80 driver does, but configure
      // the peripheral in RGB timing mode below.
      PERIPH_RCC_ACQUIRE_ATOMIC(lcd_periph_i80_signals.buses[0].module,
                                ref_count) {
        if (ref_count == 0) {
          lcd_ll_enable_bus_clock(0, true);
          lcd_ll_reset_register(0);
        }
      }
      lcd_hal_init(&state->lcd_hal, 0);

      PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(state->lcd_hal.dev, true);
        lcd_ll_select_clk_src(state->lcd_hal.dev, LCD_CLK_SRC_PLL160M);
        // PLL160M / 2 = 80 MHz LCD domain; /4 = exact 20 MHz PCLK.
        lcd_ll_set_group_clock_coeff(state->lcd_hal.dev, 2, 0, 0);
      }
      lcd_ll_set_pixel_clock_prescale(state->lcd_hal.dev, 4);
      lcd_ll_reset(state->lcd_hal.dev);
      lcd_ll_fifo_reset(state->lcd_hal.dev);
      lcd_ll_set_clock_idle_level(state->lcd_hal.dev, false);
      lcd_ll_set_pixel_clock_edge(state->lcd_hal.dev, false);
      lcd_ll_enable_rgb_mode(state->lcd_hal.dev, true);
      lcd_ll_set_dma_read_stride(state->lcd_hal.dev, state->bus.bus_width);
      lcd_ll_set_data_wire_width(state->lcd_hal.dev, state->bus.bus_width);
      lcd_ll_set_phase_cycles(state->lcd_hal.dev, 0, 0, 1);
      lcd_ll_enable_output_always_on(state->lcd_hal.dev, false);
      // LE is active high, STH/DE is active low, matching the working I80 CS.
      lcd_ll_set_idle_level(state->lcd_hal.dev, false, true, true);
      lcd_ll_set_blank_cycles(state->lcd_hal.dev, 1, 1);
      lcd_ll_enable_output_hsync_in_porch_region(state->lcd_hal.dev, false);
      lcd_ll_enable_auto_next_frame(state->lcd_hal.dev, false);

      const uint32_t total_line_clocks =
          (MYPAPER_EPD_2D_PIXEL_CLOCK_HZ / 1000000u)
          * state->line_period_us;
      const uint32_t fixed_clocks = MYPAPER_EPD_2D_LE_HIGH_PCLKS
                                  + MYPAPER_EPD_2D_LINE_FRONT_PCLKS
                                  + state->active_words;
      if (total_line_clocks <= fixed_clocks) {
        ESP_LOGE(TAG, "invalid line timing: total=%lu fixed=%lu",
                 (unsigned long)total_line_clocks,
                 (unsigned long)fixed_clocks);
        return false;
      }
      const uint32_t remaining_clocks = total_line_clocks - fixed_clocks;
      lcd_ll_set_horizontal_timing(state->lcd_hal.dev,
                                   MYPAPER_EPD_2D_LE_HIGH_PCLKS,
                                   MYPAPER_EPD_2D_LINE_FRONT_PCLKS,
                                   state->active_words,
                                   remaining_clocks);
      lcd_ll_set_vertical_timing(state->lcd_hal.dev, 1, 0, state->rows, 1);
      lcd_ll_set_hsync_position(state->lcd_hal.dev, 1);

      static constexpr int data_signals[16] = {
        LCD_DATA_OUT_PAD_OUT0_IDX,  LCD_DATA_OUT_PAD_OUT1_IDX,
        LCD_DATA_OUT_PAD_OUT2_IDX,  LCD_DATA_OUT_PAD_OUT3_IDX,
        LCD_DATA_OUT_PAD_OUT4_IDX,  LCD_DATA_OUT_PAD_OUT5_IDX,
        LCD_DATA_OUT_PAD_OUT6_IDX,  LCD_DATA_OUT_PAD_OUT7_IDX,
        LCD_DATA_OUT_PAD_OUT8_IDX,  LCD_DATA_OUT_PAD_OUT9_IDX,
        LCD_DATA_OUT_PAD_OUT10_IDX, LCD_DATA_OUT_PAD_OUT11_IDX,
        LCD_DATA_OUT_PAD_OUT12_IDX, LCD_DATA_OUT_PAD_OUT13_IDX,
        LCD_DATA_OUT_PAD_OUT14_IDX, LCD_DATA_OUT_PAD_OUT15_IDX,
      };
      for (unsigned i = 0; i < state->bus.bus_width; ++i) {
        connect_lcd_signal(state->bus.pin_data[i], data_signals[i]);
      }
      connect_lcd_signal(state->bus.pin_cl, LCD_PCLK_PAD_OUT_IDX);
      connect_lcd_signal(state->bus.pin_le, LCD_H_SYNC_PAD_OUT_IDX);
      connect_lcd_signal(state->bus.pin_sph, LCD_H_ENABLE_PAD_OUT_IDX);
      gpio_set_direction(static_cast<gpio_num_t>(state->bus.pin_spv), GPIO_MODE_OUTPUT);
      gpio_set_level(static_cast<gpio_num_t>(state->bus.pin_spv), 1);

      lcd_ll_enable_interrupt(state->lcd_hal.dev, LCD_LL_EVENT_VSYNC_END, true);
      lcd_ll_clear_interrupt_status(state->lcd_hal.dev, UINT32_MAX);
      const int flags = ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_LOWMED;
      const esp_err_t err = esp_intr_alloc_intrstatus(
          ETS_LCD_CAM_INTR_SOURCE, flags,
          reinterpret_cast<uint32_t>(lcd_ll_get_interrupt_status_reg(state->lcd_hal.dev)),
          LCD_LL_EVENT_VSYNC_END, lcd_frame_isr, state, &state->lcd_interrupt);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD interrupt allocation failed: %s", esp_err_to_name(err));
        return false;
      }
      return true;
    }
  }

  bool epd2d_continuous_scan_init(const Bus_EPD::config_t& config,
                                  size_t panel_width, size_t panel_height)
  {
    auto* state = &scan_state;
    if (state->initialized) {
      return true;
    }
    if (config.bus_width != 16 || (panel_width & 7) != 0
     || panel_width == 0 || panel_height == 0) {
      ESP_LOGE(TAG, "unsupported continuous geometry: %ux%u bus=%u",
               (unsigned)panel_width, (unsigned)panel_height,
               (unsigned)config.bus_width);
      return false;
    }

    state->bus = config;
    state->active_words = panel_width >> 3; // eight 2-bit EPD pixels per 16-bit word
    state->rows = panel_height;
    state->frame_bytes = state->active_words * sizeof(uint16_t) * state->rows;
    state->line_period_us = MYPAPER_EPD_2D_LINE_PERIOD_US;
    state->frame_done = xSemaphoreCreateBinary();
    if (state->frame_done == nullptr) {
      ESP_LOGE(TAG, "frame semaphore allocation failed");
      return false;
    }
    if (!init_dma(state)
#if MYPAPER_EPD_2D_LINE_STREAM
     || !init_streaming(state)
#endif
     || !init_lcd(state) || !init_ckv(state)) {
      return false;
    }
    state->initialized = true;
    ESP_LOGI(TAG,
             "continuous scan ready: active=%u words rows=%u frame=%u bytes pclk=%lu Hz line=%lu us CKV=%lu.%lu us",
             (unsigned)state->active_words, (unsigned)state->rows,
             (unsigned)state->frame_bytes,
             (unsigned long)MYPAPER_EPD_2D_PIXEL_CLOCK_HZ,
             (unsigned long)state->line_period_us,
             (unsigned long)(MYPAPER_EPD_2D_CKV_HIGH_DUS / 10),
             (unsigned long)(MYPAPER_EPD_2D_CKV_HIGH_DUS % 10));
    return true;
  }

  bool epd2d_continuous_scan_set_line_period_us(uint32_t line_period_us)
  {
    auto* state = &scan_state;
    if (!state->initialized) {
      ESP_LOGW(TAG, "line timing change rejected before scan initialization");
      return false;
    }
    const uint32_t clocks_per_us = MYPAPER_EPD_2D_PIXEL_CLOCK_HZ / 1000000u;
    const uint32_t fixed_clocks = MYPAPER_EPD_2D_LE_HIGH_PCLKS
                                + MYPAPER_EPD_2D_LINE_FRONT_PCLKS
                                + state->active_words;
    const uint32_t total_line_clocks = clocks_per_us * line_period_us;
    if (line_period_us < 10 || line_period_us > 30
     || total_line_clocks <= fixed_clocks) {
      ESP_LOGE(TAG, "invalid runtime line timing: %lu us total=%lu fixed=%lu",
               (unsigned long)line_period_us, (unsigned long)total_line_clocks,
               (unsigned long)fixed_clocks);
      return false;
    }
    if (state->line_period_us == line_period_us) {
      return true;
    }

    // All callers are required to wait for the preceding phase.  Stop the
    // LCD first so the timing registers cannot be changed during a scan.
    lcd_ll_stop(state->lcd_hal.dev);
    lcd_ll_set_horizontal_timing(state->lcd_hal.dev,
                                 MYPAPER_EPD_2D_LE_HIGH_PCLKS,
                                 MYPAPER_EPD_2D_LINE_FRONT_PCLKS,
                                 state->active_words,
                                 total_line_clocks - fixed_clocks);
    state->line_period_us = line_period_us;
    ESP_LOGI(TAG, "scan timing set: line=%lu us phase_nominal=%lu us (%.1f Hz)",
             (unsigned long)line_period_us,
             (unsigned long)(line_period_us * state->rows),
             1000000.0f / (float)(line_period_us * state->rows));
    return true;
  }

  uint32_t epd2d_continuous_scan_get_line_period_us()
  {
    return scan_state.initialized ? scan_state.line_period_us : 0;
  }

  bool epd2d_continuous_scan_frame(const uint8_t* data, size_t length,
                                   uint32_t timeout_ms,
                                   uint32_t* scan_time_us)
  {
    auto* state = &scan_state;
    if (scan_time_us) { *scan_time_us = 0; }
    if (!state->initialized || data == nullptr || length != state->frame_bytes) {
      ESP_LOGE(TAG, "invalid frame: ready=%u data=%p length=%u expected=%u",
               state->initialized, data, (unsigned)length,
               (unsigned)state->frame_bytes);
      return false;
    }

    // A monolithic PSRAM cache writeback can hold the shared cache controller
    // longer than one 125 us USB HS microframe. Keep each non-preemptible cache
    // operation short so UAC2 can submit the next isochronous URB on time.
    esp_err_t err = ESP_OK;
    constexpr size_t sync_chunk = 16 * 1024;
    for (size_t offset = 0; offset < length; offset += sync_chunk) {
      const size_t chunk = std::min(sync_chunk, length - offset);
      err = esp_cache_msync(const_cast<uint8_t*>(data) + offset, chunk,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M
                          | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame cache writeback failed at %u/%u: %s",
                 (unsigned)offset, (unsigned)length, esp_err_to_name(err));
        return false;
      }
    }
    gdma_buffer_mount_config_t mount = {};
    mount.buffer = const_cast<uint8_t*>(data);
    mount.buffer_alignment = state->dma_alignment;
    mount.length = length;
    mount.flags.mark_eof = true;
    mount.flags.mark_final = true;
    if ((err = gdma_link_mount_buffers(state->dma_links, 0, &mount, 1, nullptr)) != ESP_OK) {
      ESP_LOGE(TAG, "frame DMA mount failed: %s", esp_err_to_name(err));
      return false;
    }

    while (xSemaphoreTake(state->frame_done, 0) == pdTRUE) {}
    gdma_reset(state->dma_channel);
    lcd_ll_stop(state->lcd_hal.dev);
    lcd_ll_fifo_reset(state->lcd_hal.dev);
    lcd_ll_clear_interrupt_status(state->lcd_hal.dev, UINT32_MAX);
    lcd_ll_enable_auto_next_frame(state->lcd_hal.dev, false);
    if ((err = gdma_start(state->dma_channel,
                          gdma_link_get_head_addr(state->dma_links))) != ESP_OK) {
      ESP_LOGE(TAG, "gdma_start failed: %s", esp_err_to_name(err));
      return false;
    }

    const uint32_t ckv_low_dus = state->line_period_us * 10u
                               - MYPAPER_EPD_2D_CKV_HIGH_DUS;
    const rmt_symbol_word_t ckv_symbol = {
      .duration0 = MYPAPER_EPD_2D_CKV_HIGH_DUS,
      .level0 = 1,
      .duration1 = static_cast<uint16_t>(ckv_low_dus),
      .level1 = 0,
    };
    rmt_transmit_config_t transmit = {};
    transmit.loop_count = state->rows + 5;
    transmit.flags.eot_level = 0;

    const int64_t scan_begin = esp_timer_get_time();
    gpio_set_level(static_cast<gpio_num_t>(state->bus.pin_spv), 0);
    if ((err = rmt_transmit(state->ckv_channel, state->ckv_encoder,
                            &ckv_symbol, sizeof(ckv_symbol), &transmit)) != ESP_OK) {
      gpio_set_level(static_cast<gpio_num_t>(state->bus.pin_spv), 1);
      ESP_LOGE(TAG, "CKV start failed: %s", esp_err_to_name(err));
      return false;
    }
    // Match epdiy's frame-start sequence: STV low for one row, high for one
    // row, then start active LCD data near the end of the following CKV-high.
    esp_rom_delay_us(state->line_period_us);
    gpio_set_level(static_cast<gpio_num_t>(state->bus.pin_spv), 1);
    esp_rom_delay_us(state->line_period_us);
    esp_rom_delay_us(MYPAPER_EPD_2D_CKV_HIGH_DUS / 10u);
    lcd_ll_start(state->lcd_hal.dev);

    const bool completed = xSemaphoreTake(state->frame_done,
                                           pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    lcd_ll_stop(state->lcd_hal.dev);
    gpio_set_level(static_cast<gpio_num_t>(state->bus.pin_spv), 1);
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - scan_begin);
    if (scan_time_us) { *scan_time_us = elapsed; }
    const esp_err_t rmt_wait = rmt_tx_wait_all_done(state->ckv_channel, 20);
    if (!completed) {
      ESP_LOGE(TAG, "continuous frame timeout after %lu us",
               (unsigned long)elapsed);
      return false;
    }
    if (rmt_wait != ESP_OK) {
      ESP_LOGW(TAG, "CKV completion wait: %s", esp_err_to_name(rmt_wait));
    }
    return true;
  }

  bool epd2d_continuous_scan_stream(
      Bus_EPD::epd_2d_line_renderer_t renderer, void* context,
      uint32_t timeout_ms, Bus_EPD::epd_2d_stream_stats_t* stats)
  {
    auto* state = &scan_state;
    if (stats) { *stats = {}; }
    if (!state->initialized || renderer == nullptr
     || state->stream_dma_links == nullptr) {
      ESP_LOGE(TAG, "invalid stream request: ready=%u renderer=%p links=%p",
               state->initialized, renderer, state->stream_dma_links);
      return false;
    }

    while (xSemaphoreTake(state->frame_done, 0) == pdTRUE) {}
    for (unsigned i = 0; i < 2; ++i) {
      while (xSemaphoreTake(state->producer_done[i], 0) == pdTRUE) {}
      while (xSemaphoreTake(state->producer_space[i], 0) == pdTRUE) {}
      __atomic_store_n(&state->queue_read[i], 0, __ATOMIC_RELAXED);
      __atomic_store_n(&state->queue_write[i], 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&state->lines_prepared, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state->lines_consumed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state->bounce_eof_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state->stream_underruns, 0, __ATOMIC_RELAXED);
    state->line_renderer = renderer;
    state->line_context = context;
    state->producer_abort = false;

    const int64_t prefetch_begin = esp_timer_get_time();
    xTaskNotifyGive(state->producer_tasks[0]);
    xTaskNotifyGive(state->producer_tasks[1]);
    const uint32_t prefetch_limit_us = std::max<uint32_t>(50000,
                                                          timeout_ms * 1000u);
    while (__atomic_load_n(&state->lines_prepared, __ATOMIC_ACQUIRE)
             < STREAM_PREFETCH_LINES) {
      if (esp_timer_get_time() - prefetch_begin >= prefetch_limit_us) {
        stop_stream_producers(state);
        ESP_LOGE(TAG, "line stream prefetch timeout: prepared=%lu/%u",
                 (unsigned long)__atomic_load_n(&state->lines_prepared,
                                                 __ATOMIC_RELAXED),
                 (unsigned)STREAM_PREFETCH_LINES);
        return false;
      }
      taskYIELD();
    }
    const uint32_t prefetch_us = static_cast<uint32_t>(
        esp_timer_get_time() - prefetch_begin);

    // Seed both bounce buffers before GDMA owns them.  The remaining queued
    // lines provide producer jitter headroom once the LCD starts consuming.
    stream_fill_bounce(state, state->bounce_buffers[0]);
    stream_fill_bounce(state, state->bounce_buffers[1]);
    // Seeding consumed four rows from each queue before the DMA ISR exists.
    // Let both producers refill that space while the scan is starting.
    xSemaphoreGive(state->producer_space[0]);
    xSemaphoreGive(state->producer_space[1]);

    gdma_reset(state->dma_channel);
    lcd_ll_stop(state->lcd_hal.dev);
    lcd_ll_fifo_reset(state->lcd_hal.dev);
    lcd_ll_clear_interrupt_status(state->lcd_hal.dev, UINT32_MAX);
    lcd_ll_enable_auto_next_frame(state->lcd_hal.dev, false);
    esp_err_t err = gdma_start(state->dma_channel,
                               gdma_link_get_head_addr(state->stream_dma_links));
    if (err != ESP_OK) {
      stop_stream_producers(state);
      ESP_LOGE(TAG, "stream gdma_start failed: %s", esp_err_to_name(err));
      return false;
    }

    const uint32_t ckv_low_dus = state->line_period_us * 10u
                               - MYPAPER_EPD_2D_CKV_HIGH_DUS;
    const rmt_symbol_word_t ckv_symbol = {
      .duration0 = MYPAPER_EPD_2D_CKV_HIGH_DUS,
      .level0 = 1,
      .duration1 = static_cast<uint16_t>(ckv_low_dus),
      .level1 = 0,
    };
    rmt_transmit_config_t transmit = {};
    transmit.loop_count = state->rows + 5;
    transmit.flags.eot_level = 0;

    state->stream_active = true;
    const int64_t scan_begin = esp_timer_get_time();
    gpio_set_level(static_cast<gpio_num_t>(state->bus.pin_spv), 0);
    err = rmt_transmit(state->ckv_channel, state->ckv_encoder,
                       &ckv_symbol, sizeof(ckv_symbol), &transmit);
    if (err == ESP_OK) {
      esp_rom_delay_us(state->line_period_us);
      gpio_set_level(static_cast<gpio_num_t>(state->bus.pin_spv), 1);
      esp_rom_delay_us(state->line_period_us);
      esp_rom_delay_us(MYPAPER_EPD_2D_CKV_HIGH_DUS / 10u);
      lcd_ll_start(state->lcd_hal.dev);
    }

    const bool completed = err == ESP_OK
        && xSemaphoreTake(state->frame_done,
                          pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    state->stream_active = false;
    lcd_ll_stop(state->lcd_hal.dev);
    gdma_reset(state->dma_channel);
    gpio_set_level(static_cast<gpio_num_t>(state->bus.pin_spv), 1);
    const uint32_t scan_us = static_cast<uint32_t>(
        esp_timer_get_time() - scan_begin);
    rmt_tx_wait_all_done(state->ckv_channel, 20);

    // Stop a producer that did not finish before VSYNC and release it from a
    // full queue.  This also makes a failed phase safe for the next request.
    stop_stream_producers(state);

    const uint32_t underruns = __atomic_load_n(&state->stream_underruns,
                                                __ATOMIC_RELAXED);
    if (stats) {
      stats->prefetch_time_us = prefetch_us;
      stats->scan_time_us = scan_us;
      stats->underruns = underruns;
    }
    if (!completed) {
      ESP_LOGE(TAG, "line stream frame failed: rmt=%s elapsed=%lu us",
               esp_err_to_name(err), (unsigned long)scan_us);
      return false;
    }
    if (underruns != 0) {
      ESP_LOGE(TAG, "line stream underrun: lines=%lu prepared=%lu consumed=%lu",
               (unsigned long)underruns,
               (unsigned long)__atomic_load_n(&state->lines_prepared,
                                               __ATOMIC_RELAXED),
               (unsigned long)__atomic_load_n(&state->lines_consumed,
                                               __ATOMIC_RELAXED));
      return false;
    }
    return true;
  }

 }
}

#else

namespace lgfx
{
 inline namespace v1
 {
  bool epd2d_continuous_scan_init(const Bus_EPD::config_t&, size_t, size_t)
  {
    return false;
  }

  bool epd2d_continuous_scan_set_line_period_us(uint32_t)
  {
    return false;
  }

  uint32_t epd2d_continuous_scan_get_line_period_us()
  {
    return 0;
  }

  bool epd2d_continuous_scan_frame(const uint8_t*, size_t, uint32_t, uint32_t*)
  {
    return false;
  }

  bool epd2d_continuous_scan_stream(
      Bus_EPD::epd_2d_line_renderer_t, void*, uint32_t,
      Bus_EPD::epd_2d_stream_stats_t*)
  {
    return false;
  }
 }
}

#endif
