#pragma once

// Skip PSRAM state reads for inactive 16-pixel groups. The original full-row
// blitter remains available as an allocation/self-test fallback.
#ifndef MYPAPER_EPD_USE_ACTIVE_GROUP_MAP
#define MYPAPER_EPD_USE_ACTIVE_GROUP_MAP 1
#endif

// Sparse rows can finish before the preceding ~6 us I80 DMA transfer. Avoid
// paying a full scheduler yield for that short gap; retain a periodic yield as
// a stalled-transfer safety fallback.
#ifndef MYPAPER_EPD_USE_P4_SHORT_I80_POLL
#define MYPAPER_EPD_USE_P4_SHORT_I80_POLL 1
#endif

// Temporary aggregate profiling for the 684 per-line I80 transactions. This
// adds no per-spin timing calls; only one timestamp is taken around each wait
// and submit operation.
#ifndef MYPAPER_EPD_PROFILE_I80
#define MYPAPER_EPD_PROFILE_I80 0
#endif

// Keep the differential engine in a separate panel implementation.  Setting
// this to 0 restores the legacy Panel_EPD path without changing either engine.
#ifndef MYPAPER_EPD_USE_2D_ENGINE
#define MYPAPER_EPD_USE_2D_ENGINE 1
#endif

// ESP32-P4-only scan backend for the differential engine.  It switches the
// LCD_CAM peripheral to RGB timing mode and emits one complete phase as a
// continuous DMA frame.  The legacy Panel_EPD path never enters this branch.
#ifndef MYPAPER_EPD_2D_CONTINUOUS_SCAN
#define MYPAPER_EPD_2D_CONTINUOUS_SCAN 1
#endif

// Build every drive frame before the panel is powered, then submit the phase
// sequence without CPU rendering gaps.  A 48-phase 1216x684 GC16 waveform uses
// about 9.52 MiB of temporary PSRAM, released immediately after the batch.
#ifndef MYPAPER_EPD_2D_PRECOMPUTE_WAVEFORM
#define MYPAPER_EPD_2D_PRECOMPUTE_WAVEFORM 1
#endif

// DU/Fast keeps the official phase sequence but stores only distinct drive
// frames for the old->new grayscale transitions present in this update.
// Equal phases reuse the same DMA frame; GC16/Quality is unaffected.
#ifndef MYPAPER_EPD_2D_DU_PHASE_DEDUP
#define MYPAPER_EPD_2D_DU_PHASE_DEDUP 1
#endif

// Experimental epdiy-style line producer/consumer backend.  Keep disabled:
// the current P4 circular-GDMA implementation consumes its software queues
// but does not reliably present the refilled bounce data to the panel.
// Whole-phase precomputation is the validated Fast path.
#ifndef MYPAPER_EPD_2D_LINE_STREAM
#define MYPAPER_EPD_2D_LINE_STREAM 0
#endif

// Experimental GC16-only balance correction.  The supplied 0->0 sequence
// first drives white for 18 frames but returns black for only 17, which leaves
// a light outline inside a black target.  Reuse idle phases immediately before
// the final idle frame for black compensation; DU and all other transitions
// remain byte-for-byte unchanged.  Set to 0 to restore the official table.
#ifndef MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES
#define MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES 2
#endif

// Official E0470A01/ED047TC2-1216 timing: a 20 MHz source clock, 320 PCLKs per
// row (16 us), with CKV high for 6 us.  Keep these as compile-time controls so
// timing experiments do not touch the legacy driver.
#ifndef MYPAPER_EPD_2D_PIXEL_CLOCK_HZ
#define MYPAPER_EPD_2D_PIXEL_CLOCK_HZ 20000000u
#endif
#ifndef MYPAPER_EPD_2D_LINE_PERIOD_US
#define MYPAPER_EPD_2D_LINE_PERIOD_US 20u
#endif
#ifndef MYPAPER_EPD_2D_CKV_HIGH_DUS
#define MYPAPER_EPD_2D_CKV_HIGH_DUS 60u
#endif
#ifndef MYPAPER_EPD_2D_LE_HIGH_PCLKS
#define MYPAPER_EPD_2D_LE_HIGH_PCLKS 4u
#endif
#ifndef MYPAPER_EPD_2D_LINE_FRONT_PCLKS
#define MYPAPER_EPD_2D_LINE_FRONT_PCLKS 4u
#endif

#define MYPAPER_EPD_PANEL_ED052TC4 0
#define MYPAPER_EPD_PANEL_E0470A01 1

#ifndef MYPAPER_EPD_PANEL
#define MYPAPER_EPD_PANEL MYPAPER_EPD_PANEL_E0470A01
#endif

#if MYPAPER_EPD_PANEL == MYPAPER_EPD_PANEL_E0470A01
#define MYPAPER_EPD_PANEL_NAME "E0470A01-AF-S"
#define MYPAPER_EPD_BUS_WIDTH 16
#define MYPAPER_EPD_LINE_PADDING 10
#define MYPAPER_EPD_MEMORY_WIDTH 1216
#define MYPAPER_EPD_PANEL_WIDTH 1216
#define MYPAPER_EPD_MEMORY_HEIGHT 684
#define MYPAPER_EPD_PANEL_HEIGHT 684
#define MYPAPER_EPD_OFFSET_ROTATION 3
#define MYPAPER_EPD_VCOM_MV 1040
#define MYPAPER_EPD_OUTPUT_PERMUTE 1

#elif MYPAPER_EPD_PANEL == MYPAPER_EPD_PANEL_ED052TC4
#define MYPAPER_EPD_PANEL_NAME "ED052TC4"
#define MYPAPER_EPD_BUS_WIDTH 8
#define MYPAPER_EPD_LINE_PADDING 8
#define MYPAPER_EPD_MEMORY_WIDTH 1280
#define MYPAPER_EPD_PANEL_WIDTH 1280
#define MYPAPER_EPD_MEMORY_HEIGHT 720
#define MYPAPER_EPD_PANEL_HEIGHT 720
#define MYPAPER_EPD_OFFSET_ROTATION 5
#define MYPAPER_EPD_VCOM_MV 2560
#define MYPAPER_EPD_OUTPUT_PERMUTE 0

#else
#error "Unsupported MYPAPER_EPD_PANEL"
#endif
