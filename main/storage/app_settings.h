#pragma once
#include "parser/book_parser.h"
#include <stdbool.h>

/**
 * Persistent application settings (stored as JSON on SD card).
 */
bool app_settings_load(const char *path);
bool app_settings_save(const char *path);

reader_settings_t app_settings_get(void);
void app_settings_set(const reader_settings_t *s);
