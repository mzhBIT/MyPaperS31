#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>
#include <sdkconfig.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    CELLULAR_OFF, CELLULAR_STARTING, CELLULAR_SEARCHING, CELLULAR_ONLINE,
    CELLULAR_NO_SIM, CELLULAR_REGISTRATION_DENIED, CELLULAR_ERROR, CELLULAR_STOPPING,
    CELLULAR_NOT_PRESENT
} cellular_state_t;
typedef struct {
    cellular_state_t state;
    int sim_slot; // Modem-confirmed: 0 external, 1 internal, -1 unknown.
    int requested_slot;
    int csq; // 0..31, 99 unknown. Cached; no AT traffic from UI.
    esp_err_t error;
    uint32_t revision;
} cellular_status_t;

#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_MYPAPER_S31_CELLULAR
// Normal system boot only, before any network worker starts. No registration.
esp_err_t cellular_network_probe_at_boot(void);
bool cellular_network_available(void); // Cached physical detection, not SIM/IP.
esp_err_t cellular_network_load_config(void);
esp_err_t cellular_network_start(void);
esp_err_t cellular_network_retry(void);
esp_err_t cellular_network_stop(int timeout_ms);
esp_err_t cellular_network_wait_online(int timeout_ms);
bool cellular_network_is_online(void);
void cellular_network_get_status(cellular_status_t *status);
esp_err_t cellular_network_set_sim_slot(int slot);
// Block new starts while standby/shutdown owns the hardware.
esp_err_t cellular_network_suspend(void);
void cellular_network_resume(void);
#elif CONFIG_IDF_TARGET_ESP32S31
static inline esp_err_t cellular_network_probe_at_boot(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline bool cellular_network_available(void) { return false; }
static inline esp_err_t cellular_network_load_config(void) { return ESP_OK; }
static inline esp_err_t cellular_network_start(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t cellular_network_retry(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t cellular_network_stop(int timeout_ms) { (void)timeout_ms; return ESP_OK; }
static inline esp_err_t cellular_network_wait_online(int timeout_ms) { (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED; }
static inline bool cellular_network_is_online(void) { return false; }
static inline void cellular_network_get_status(cellular_status_t *s) {
    if (s) { s->state=CELLULAR_NOT_PRESENT; s->sim_slot=-1; s->requested_slot=-1;
             s->csq=99; s->error=ESP_ERR_NOT_SUPPORTED; s->revision=0; }
}
static inline esp_err_t cellular_network_set_sim_slot(int slot) { (void)slot; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t cellular_network_suspend(void) { return ESP_OK; }
static inline void cellular_network_resume(void) {}
#else
#include "usb/usb_modem_probe.h"
static inline esp_err_t cellular_network_load_config(void) { return ESP_OK; }
static inline esp_err_t cellular_network_start(void) { return usb_modem_probe_start(); }
static inline esp_err_t cellular_network_stop(int timeout_ms) { (void)timeout_ms; return ESP_OK; }
static inline esp_err_t cellular_network_wait_online(int timeout_ms) { return usb_modem_probe_wait_online(timeout_ms); }
static inline bool cellular_network_is_online(void) { return usb_modem_probe_is_online(); }
#endif
#ifdef __cplusplus
}
#endif
