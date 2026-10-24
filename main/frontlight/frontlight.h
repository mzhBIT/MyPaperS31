#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the board-specific frontlight and restores the saved 0..10 level.
void frontlight_init(void);

// Level 0 is off. Both boards: 5% per level (max 50%).
int frontlight_get_level(void);
int frontlight_get_percent(void);
void frontlight_set_level(int level);

// Relative CCT blend: 0=warm, 5=neutral, 10=cool. Not calibrated Kelvin.
// Returns -1 on single-channel boards; setting it there is a no-op.
int frontlight_get_color_temperature(void);
void frontlight_set_color_temperature(int level);

#ifdef __cplusplus
}
#endif
