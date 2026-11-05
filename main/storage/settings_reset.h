#pragma once
#include <esp_err.h>

// Restart after success to discard in-memory preference caches.
// User content, account sessions and calibration are preserved.
// Wi-Fi radio/driver configuration and saved credentials are cleared; credential
// absence is read back before success is reported.
esp_err_t settings_restore_defaults();
