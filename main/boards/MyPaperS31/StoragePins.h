#pragma once

// SD card wiring for MyPaperS31, C-compatible so main/storage/storage_sdcard.c
// can be shared with the P4 implementation. Keep in sync with the schematic
// and with Pins.h, which exposes the same values to C++ code.
#define MYPAPER_S31_SD_CLK 24
#define MYPAPER_S31_SD_CMD 25
#define MYPAPER_S31_SD_D0  20
#define MYPAPER_S31_SD_D1  21
#define MYPAPER_S31_SD_D2  22
#define MYPAPER_S31_SD_D3  23
