#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The configured file is stored in NVS. An empty path disables the ringtone.
esp_err_t phone_ringtone_set_path(const char *path);
esp_err_t phone_ringtone_get_path(char *path, unsigned size);
void phone_ringtone_start(void);
void phone_ringtone_stop(void);

#ifdef __cplusplus
}
#endif
