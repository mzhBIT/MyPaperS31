#pragma once

// S31 RGB transfers have passed real-board validation. Bound GC16/HD PSRAM
// use with a 16-slot ring rather than allocating all distinct drive frames.
// Start with full-ring preheat until concurrent producer throughput is tested.
// S31 keeps 340 RGB clocks per row: 21.25 MHz gives P4's 16 us period.
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#define MYPAPER_EPD_2D_GC16_PIPELINE 1
#define MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS 16
#define MYPAPER_EPD_2D_GC16_PIPELINE_PRIME 16
#define MYPAPER_EPD_2D_HD_PIPELINE_PRIME 16
#define MYPAPER_EPD_2D_DU4_PIPELINE 1
#define MYPAPER_EPD_2D_PIXEL_CLOCK_HZ 21250000u
#define MYPAPER_EPD_2D_LINE_PERIOD_US 16u
#define MYPAPER_EPD_2D_CKV_HIGH_DUS 134u
#endif

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

// DU/A2 target quantization is mandatory in the 2D refresh task. Drawing
// always preserves gray4; no switch may leave binary waveform target rows
// 1..14 reachable. The drawing framebuffer is not modified by quantization.

// Expand the current GC16 phase's 16x16 pixel transition table into a direct
// old-byte/new-byte lookup.  The 64 KiB table lives in internal RAM and turns
// two pixel LUT reads plus masking/shifting into one lookup for each pair of
// 4bpp pixels.  The official waveform data and phase timing are unchanged.
#ifndef MYPAPER_EPD_2D_GC16_PAIR_LUT
#define MYPAPER_EPD_2D_GC16_PAIR_LUT 1
#endif

// GC16/HD index reuse, including partial updates. Set 0 for the original path.
#ifndef MYPAPER_EPD_2D_GC16_PAIR_INDICES
#define MYPAPER_EPD_2D_GC16_PAIR_INDICES 1
#endif

// Pair preheat frames in one 64 KiB LUT; set 0 for sequential A/B comparison.
#ifndef MYPAPER_EPD_2D_GC16_DUAL_PRIME
#define MYPAPER_EPD_2D_GC16_DUAL_PRIME 1
#endif

// Pair background frames when two reusable ring slots are already available.
// Keep prime/slot counts unchanged; set 0 to compare the original producer.
#ifndef MYPAPER_EPD_2D_GC16_DUAL_WORKER
#define MYPAPER_EPD_2D_GC16_DUAL_WORKER 1
#endif

// Prime a bounded GC16 frame ring before power-on, then render the remaining
// unique phases on the same CPU core while GDMA scans the prepared frames.
// The scan task has the higher priority and therefore keeps phase boundaries
// deterministic. DU4 uses its separate pipeline and priming controls below.
#ifndef MYPAPER_EPD_2D_GC16_PIPELINE
#define MYPAPER_EPD_2D_GC16_PIPELINE 1
#endif
#ifndef MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS
#define MYPAPER_EPD_2D_GC16_PIPELINE_SLOTS 16
#endif
// Fixed preheat, including dense GC16 batches. For a larger A/B startup margin,
// raise prime and ring capacity together; prime must not exceed ring capacity.
#ifndef MYPAPER_EPD_2D_GC16_PIPELINE_PRIME
#define MYPAPER_EPD_2D_GC16_PIPELINE_PRIME 10
#endif

// HD independently tests 8-frame preheat; it shares the 16-slot ring.
#ifndef MYPAPER_EPD_2D_HD_PIPELINE_PRIME
#define MYPAPER_EPD_2D_HD_PIPELINE_PRIME 8
#endif

// Fuse the first three full-screen DU4 drive frames. Allocation/geometry
// failure falls back to the established per-frame renderer.
#ifndef MYPAPER_EPD_2D_DU4_TRIPLE_PRIME
#define MYPAPER_EPD_2D_DU4_TRIPLE_PRIME 1
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

// Reuse the GC16 index workspace with DU4 12-bit keys for full-screen batches.
// Independent switches permit A/B testing without changing waveforms/priming.
#ifndef MYPAPER_EPD_2D_DU4_PAIR_INDICES
#define MYPAPER_EPD_2D_DU4_PAIR_INDICES 1
#endif
#ifndef MYPAPER_EPD_2D_DU4_DUAL_WORKER
#define MYPAPER_EPD_2D_DU4_DUAL_WORKER 1
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

// GC16 0->0 A/B adjustment: turn the last six white-drive phases (22..27)
// into black drive. This keeps 48 phases and changes white18/black17 to
// white12/black23 without touching other transitions or the 36-phase test.
#ifndef MYPAPER_EPD_2D_GC16_00_REBALANCE
#define MYPAPER_EPD_2D_GC16_00_REBALANCE 1
#endif

// GC16 0->1 A/B adjustment: turn white-drive phases 21..26 into black drive.
// Keep the original final white phase 44 and all other transitions intact.
#ifndef MYPAPER_EPD_2D_GC16_01_REBALANCE
#define MYPAPER_EPD_2D_GC16_01_REBALANCE 1
#endif

// GC16 0->2 A/B adjustment: turn white-drive phases 22..24 into black drive.
// Preserve the original alternating tail and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_02_REBALANCE
#define MYPAPER_EPD_2D_GC16_02_REBALANCE 1
#endif

// GC16 0->3 A/B adjustment: turn white-drive phase 24 into black drive.
// Preserve the remaining phases and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_03_REBALANCE
#define MYPAPER_EPD_2D_GC16_03_REBALANCE 1
#endif

// GC16 0->4 A/B adjustment: turn white-drive phase 23 into black drive.
// Preserve the remaining phases and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_04_REBALANCE
#define MYPAPER_EPD_2D_GC16_04_REBALANCE 1
#endif

// GC16 0->7 A/B adjustment: turn black-drive phases 23..25 into white drive.
// Preserve the remaining phases and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_07_REBALANCE
#define MYPAPER_EPD_2D_GC16_07_REBALANCE 1
#endif

// GC16 0->8 A/B adjustment: turn black-drive phases 23..24 into white drive.
// Preserve the remaining phases and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_08_REBALANCE
#define MYPAPER_EPD_2D_GC16_08_REBALANCE 1
#endif

// GC16 0->9 A/B adjustment: turn black-drive phases 23..24 into white drive.
// Preserve the remaining phases and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_09_REBALANCE
#define MYPAPER_EPD_2D_GC16_09_REBALANCE 1
#endif

// GC16 0->10 A/B adjustment: turn black-drive phases 21..22 into white drive.
// Preserve the remaining phases and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_0A_REBALANCE
#define MYPAPER_EPD_2D_GC16_0A_REBALANCE 1
#endif

// GC16 0->11 A/B adjustment: turn black-drive phases 19..20 into white drive.
// Preserve the remaining phases and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_0B_REBALANCE
#define MYPAPER_EPD_2D_GC16_0B_REBALANCE 1
#endif

// GC16 0->12 A/B adjustment: turn black-drive phases 21..22 into white drive.
// Preserve the remaining phases and all other transitions.
#ifndef MYPAPER_EPD_2D_GC16_0C_REBALANCE
#define MYPAPER_EPD_2D_GC16_0C_REBALANCE 1
#endif

// GC16 0->13 A/B adjustment: turn black-drive phases 19..20 into white drive.
#ifndef MYPAPER_EPD_2D_GC16_0D_REBALANCE
#define MYPAPER_EPD_2D_GC16_0D_REBALANCE 1
#endif

// GC16 0->14 A/B adjustment: turn black-drive phase 18 into white drive.
#ifndef MYPAPER_EPD_2D_GC16_0E_REBALANCE
#define MYPAPER_EPD_2D_GC16_0E_REBALANCE 1
#endif

// Separate, older experiment: reuse final idle phases for more black drive.
// Leave at 0 to keep the final three idle phases; this does not disable the
// white-to-black rebalance above.
#ifndef MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES
#define MYPAPER_EPD_2D_GC16_00_BLACK_COMP_FRAMES 0
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
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#define MYPAPER_EPD_BUS_WIDTH 8
#else
#define MYPAPER_EPD_BUS_WIDTH 16
#endif
#define MYPAPER_EPD_LINE_PADDING 10
#define MYPAPER_EPD_MEMORY_WIDTH 1216
#define MYPAPER_EPD_PANEL_WIDTH 1216
#define MYPAPER_EPD_MEMORY_HEIGHT 684
#define MYPAPER_EPD_PANEL_HEIGHT 684
#define MYPAPER_EPD_OFFSET_ROTATION 3
#if defined(CONFIG_IDF_TARGET_ESP32S31)
#define MYPAPER_EPD_VCOM_MV 1230
#ifndef MYPAPER_EPD_S31_PARTIAL_OE_DEFAULT
#define MYPAPER_EPD_S31_PARTIAL_OE_DEFAULT 1
#endif
#define MYPAPER_EPD_OUTPUT_PERMUTE 0
#else
#define MYPAPER_EPD_VCOM_MV 1050
#define MYPAPER_EPD_OUTPUT_PERMUTE 1
#endif

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
