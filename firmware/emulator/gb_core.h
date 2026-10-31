#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    EMU_A = 1,
    EMU_B = 2,
    EMU_SELECT = 4,
    EMU_START = 8,
    EMU_RIGHT = 16,
    EMU_LEFT = 32,
    EMU_UP = 64,
    EMU_DOWN = 128
};
bool emu_gb_init(uint8_t *rom, size_t size);
void emu_gb_close(void);
bool emu_gb_frame(uint8_t buttons);
enum { EMU_AUDIO_RATE = 32768, EMU_AUDIO_FRAME_CAPACITY = 550 };
const int16_t *emu_gb_audio(void);
size_t emu_gb_audio_frames(void);
const uint8_t *emu_gb_lcd(void);
const char *emu_gb_error(void);
const char *emu_gb_title(void);
void emu_gb_tick(uint32_t seconds);
size_t emu_gb_battery_size(void);
bool emu_gb_battery_export(uint8_t *output, size_t size);
bool emu_gb_battery_import(const uint8_t *input, size_t size);
size_t emu_gb_state_size(void);
size_t emu_gb_legacy_state_size(void);
bool emu_gb_state_export(uint8_t *output, size_t size);
bool emu_gb_state_import(const uint8_t *input, size_t size);
#ifdef __cplusplus
}
#endif
