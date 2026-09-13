#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the dedicated USB MSC UI/event loop. Returns only on fatal setup error. */
esp_err_t usb_msc_mode_run(void);

#ifdef __cplusplus
}
#endif
