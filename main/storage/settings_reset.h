#pragma once
#include <esp_err.h>

// Restart after success to discard in-memory preference caches.
// User content, account sessions and calibration are preserved.
esp_err_t settings_restore_defaults();
