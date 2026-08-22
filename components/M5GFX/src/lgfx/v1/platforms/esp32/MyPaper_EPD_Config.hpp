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
