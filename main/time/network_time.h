#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Serialized SNTP operation; timeout includes waiting for another caller.
esp_err_t network_time_sync(uint32_t timeout_ms);
// Nonblocking IP-event hook. Starts at most one automatic attempt per boot.
void network_time_on_connected(void);

#ifdef __cplusplus
}
#endif
