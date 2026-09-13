#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool usb_transfer_boot_requested(void);
esp_err_t usb_transfer_boot_set(bool enabled);

#ifdef __cplusplus
}
#endif
