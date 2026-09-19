#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the GPIO24 PWM output and restores the saved 0..10 level.
void frontlight_init(void);

// 0 disables the PCA9535 enable output; 1..10 map to 10%..100% PWM duty.
int frontlight_get_level(void);
int frontlight_get_percent(void);
void frontlight_set_level(int level);

#ifdef __cplusplus
}
#endif
