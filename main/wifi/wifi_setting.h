#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_setting_c6_power_on(void);
esp_err_t wifi_setting_c6_power_off(void);
esp_err_t wifi_setting_init(void);
esp_err_t wifi_setting_scan(void);
esp_err_t wifi_setting_connect(const char *ssid, const char *password, int timeout_ms);
esp_err_t wifi_setting_deinit(bool power_down_c6);

#ifdef __cplusplus
}
#endif
