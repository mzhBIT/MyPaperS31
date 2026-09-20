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

// Keep the official phase sequence but store only distinct drive frames for
// the old->new grayscale transitions present in this update.  Equal phases
// reuse the same DMA frame while still being submitted for their original
// duration.  This applies to GC16, DU, DU4 and A2.
#ifndef MYPAPER_EPD_2D_DU_PHASE_DEDUP
#define MYPAPER_EPD_2D_DU_PHASE_DEDUP 1
#endif

// The supplied DU waveform has a 16-level source axis but only drives black
// and white target rows. Quantize the per-refresh target snapshot to 0/15 so
// antialiased 4bpp pixels never select the idle target rows 1..14. The main
// drawing framebuffer remains untouched for later DU4/GC16 refreshes.
#ifndef MYPAPER_EPD_2D_DU_BINARY_TARGET
#define MYPAPER_EPD_2D_DU_BINARY_TARGET 1
#endif

// Expand the current GC16 phase's 16x16 pixel transition table into a direct
// old-byte/new-byte lookup.  The 64 KiB table lives in internal RAM and turns
// two pixel LUT reads plus masking/shifting into one lookup for each pair of
// 4bpp pixels.  The official waveform data and phase timing are unchanged.
#ifndef MYPAPER_EPD_2D_GC16_PAIR_LUT
#define MYPAPER_EPD_2D_GC16_PAIR_LUT 1
#endif

// Prime a bounded GC16 frame ring before power-on, then render the remaining
// unique phases on the same CPU core while GDMA scans the prepared frames.
// The scan task has the higher priority and therefore keeps phase boundaries
// deterministic.  DU4 remains on the validated full-precompute path for now.
#ifndef MYPAPER_EPD_2D_GC16_PIPELINE
#define MYPAPER_EPD_2D_GC16_PIPELINE 1
#endif
#ifndef MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS
#define MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS 20
#endif
#ifndef MYPAPER_EPD_2D_GC16_PIPELINE_PRIME
#define MYPAPER_EPD_2D_GC16_PIPELINE_PRIME 14
#endif

// HD is the vendor 30-phase, effective eight-gray GC table. At the 16 us
// scan period one phase lasts only ~11 ms while a full pair-LUT frame takes
// longer to render. The 20-frame prefill gives the producer enough lead time
// to finish the remaining frames while scanout advances.
#ifndef MYPAPER_EPD_2D_HD_PIPELINE_PRIME
#define MYPAPER_EPD_2D_HD_PIPELINE_PRIME 10
#endif

// DU4 keeps its complete unique-frame workspace, but only renders this many
// frames before power-on.  The remaining frames are filled by a lower-priority
// task on the scan core while GDMA is shifting the already-ready frames.
#ifndef MYPAPER_EPD_2D_DU4_PIPELINE
#define MYPAPER_EPD_2D_DU4_PIPELINE 1
#endif
#ifndef MYPAPER_EPD_2D_DU4_PIPELINE_PRIME
#define MYPAPER_EPD_2D_DU4_PIPELINE_PRIME 3
#endif

// DU4 frames live in PSRAM.  Clear/render them in short bursts so the shared
// Flash/PSRAM cache bus regularly becomes available to the HS USB audio path.
// The delay is a ROM busy wait on the EPD core: it does not alter LUT contents,
// phase count, or LCD_CAM scan timing.
#ifndef MYPAPER_EPD_2D_DU4_BUS_BURST_BYTES
#define MYPAPER_EPD_2D_DU4_BUS_BURST_BYTES 4096u
#endif
#ifndef MYPAPER_EPD_2D_DU4_BUS_RELIEF_US
#define MYPAPER_EPD_2D_DU4_BUS_RELIEF_US 4u
#endif
#ifndef MYPAPER_EPD_2D_DU4_BUS_RELIEF_GROUPS
#define MYPAPER_EPD_2D_DU4_BUS_RELIEF_GROUPS 256u
#endif
#ifndef MYPAPER_EPD_2D_DU4_BUS_RELIEF_ROWS
#define MYPAPER_EPD_2D_DU4_BUS_RELIEF_ROWS 4u
#endif

// TPS65185 keeps the negative rail active for its discharge interval after a
// power-down request. Rapid UI updates can otherwise start a new high-voltage
// sequence while the previous one is still settling, occasionally leaving PG
// low. Applied only by the 2D engine's V7 power path.
#ifndef MYPAPER_EPD_2D_MIN_POWER_OFF_MS
#define MYPAPER_EPD_2D_MIN_POWER_OFF_MS 250u
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

// E0470A01/ED047TC2-1216 timing: a 20 MHz source clock and 320 PCLKs per row
// (16 us).  The horizontal start/back-porch values follow the 0.3/0.8 us
// minima, and CKV stays high for 13.4 us so the complete active-data interval
// is covered.  Keep these as compile-time controls so timing experiments do
// not touch the legacy driver.
#ifndef MYPAPER_EPD_2D_PIXEL_CLOCK_HZ
#define MYPAPER_EPD_2D_PIXEL_CLOCK_HZ 20000000u
#endif
#ifndef MYPAPER_EPD_2D_LINE_PERIOD_US
#define MYPAPER_EPD_2D_LINE_PERIOD_US 16u
#endif
#ifndef MYPAPER_EPD_2D_CKV_HIGH_DUS
#define MYPAPER_EPD_2D_CKV_HIGH_DUS 134u
#endif
#ifndef MYPAPER_EPD_2D_LE_HIGH_PCLKS
#define MYPAPER_EPD_2D_LE_HIGH_PCLKS 6u
#endif
#ifndef MYPAPER_EPD_2D_LINE_FRONT_PCLKS
#define MYPAPER_EPD_2D_LINE_FRONT_PCLKS 16u
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
#define MYPAPER_EPD_VCOM_MV 1050
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
