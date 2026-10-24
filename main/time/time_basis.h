#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Global fixed UTC offset in minutes. Defaults to Beijing time (UTC+08:00).
int time_basis_get_offset_minutes(void);
esp_err_t time_basis_set_offset_minutes(int minutes);
// ML307A may report UTC clock digits alongside an incorrect +32 zone suffix.
bool time_basis_modem_clock_is_utc(void);
esp_err_t time_basis_set_modem_clock_is_utc(bool is_utc);
bool time_basis_use_24_hour(void);
esp_err_t time_basis_set_use_24_hour(bool enabled);
bool time_basis_system_clock_valid(void);
bool time_basis_format_clock(int64_t utc_seconds, char *display, size_t capacity);
bool time_basis_parse_modem(const char *raw, int64_t *utc_seconds);
bool time_basis_parse_modem_clock(const char *raw, int64_t *utc_seconds);
bool time_basis_parse_beijing_key(int64_t key, int64_t *utc_seconds);
bool time_basis_parse_legacy_beijing(const char *display, int64_t *utc_seconds);
// Strict YYYY/MM/DD HH:MM, interpreted using the selected fixed UTC offset.
// Supported manual calendar range: 2024..2099 (system clock trust policy).
// Seconds are set to zero.
bool time_basis_parse_manual(const char *display, int64_t *utc_seconds);
bool time_basis_format_utc(int64_t utc_seconds, char *display, size_t capacity,
                           int64_t *sort_key);

#ifdef __cplusplus
}
#endif
