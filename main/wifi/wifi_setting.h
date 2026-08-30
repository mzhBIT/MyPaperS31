#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WIFI_SETTING_SSID_MAX_LEN = 32,
    WIFI_SETTING_PASSWORD_MAX_LEN = 63,
};

typedef struct {
    char ssid[WIFI_SETTING_SSID_MAX_LEN + 1];
    int8_t rssi;
    uint8_t channel;
    bool secured;
} wifi_setting_ap_t;

esp_err_t wifi_setting_c6_power_on(void);
esp_err_t wifi_setting_c6_power_off(void);
esp_err_t wifi_setting_init(void);
esp_err_t wifi_setting_scan(wifi_setting_ap_t *records,
                            size_t capacity,
                            size_t *out_count);
esp_err_t wifi_setting_connect(const char *ssid, const char *password, int timeout_ms);
esp_err_t wifi_setting_connect_saved(int timeout_ms);
bool wifi_setting_load_credentials(char *ssid,
                                   size_t ssid_size,
                                   char *password,
                                   size_t password_size);
esp_err_t wifi_setting_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_setting_clear_credentials(void);
esp_err_t wifi_setting_deinit(bool power_down_c6);

#ifdef __cplusplus
}
#endif
