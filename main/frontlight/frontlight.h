#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the board-specific frontlight and restores saved brightness.
void frontlight_init(void);

// Level 0 is off. Both boards: 5% per level (max 50%).
int frontlight_get_level(void);
int frontlight_get_percent(void);
void frontlight_set_level(int level);
// Fine brightness 0..1000 spans the existing 0..50% PWM range.
// Preview never writes flash; save once when the interaction ends.
int frontlight_get_brightness(void);
void frontlight_preview_brightness(int value);
void frontlight_save_brightness(void);
// Temporary power override; preserves user level/CCT and never writes NVS.
void frontlight_set_suspended(bool suspended);

// Relative CCT blend: 0=warm, 5=neutral, 10=cool. Not calibrated Kelvin.
// Returns -1 on single-channel boards; setting it there is a no-op.
int frontlight_get_color_temperature(void);
void frontlight_set_color_temperature(int level);
// Continuous warm/cool mix 0..1000; -1 on single-channel boards.
int frontlight_get_color_temperature_fine(void);
void frontlight_preview_color_temperature(int value);
// Saves both brightness and color temperature after a preview interaction.
void frontlight_save_settings(void);

#ifdef __cplusplus
}
#endif
