#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the ML307A RNDIS bring-up sequence.
 *
 * The service owns the selected USB Host controller, validates the known
 * 2ECC:3012 layout, uses interface 2 to establish/check the ML307A RNDIS
 * dial state, releases it, and only then starts the RNDIS host network path.
 */
esp_err_t usb_modem_probe_start(void);
bool usb_modem_probe_is_online(void);
esp_err_t usb_modem_probe_wait_online(int timeout_ms);

#ifdef __cplusplus
}
#endif
