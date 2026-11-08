#include "wifi_setting.h"
#include "S31WifiPolicy.h"
#include "time/network_time.h"

#include <stdbool.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_hosted.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "modem/cellular_network.h"

#define WIFI_SETTING_C6_EN_GPIO        GPIO_NUM_54
#define WIFI_SETTING_C6_BOOT_DELAY_MS  1000
#define WIFI_SETTING_C6_OFF_SETTLE_MS  500
#define WIFI_SETTING_MAX_AP_COUNT      24
#define WIFI_SETTING_CONNECTED_BIT     BIT0
#define WIFI_SETTING_FAIL_BIT          BIT1
#define WIFI_SETTING_STARTED_BIT       BIT2
#define WIFI_SETTING_STOPPED_BIT       BIT3
#define WIFI_SETTING_MAX_RETRY         5
#define WIFI_SETTING_STATE_TIMEOUT_MS  2000
#define WIFI_SETTING_NVS_NAMESPACE     "wifi_cfg"
#define WIFI_SETTING_NVS_SSID_KEY      "ssid"
#define WIFI_SETTING_NVS_PASSWORD_KEY  "password"
#define WIFI_SETTING_NVS_NETWORK_KEY   "network"
#define WIFI_SETTING_NVS_POWER_KEY     "power_mode"
#define WIFI_SETTING_ALWAYS_ON_TIMEOUT_MS 12000

static const char *TAG = "wifi_setting";

static bool s_netif_initialized;
static bool s_event_loop_created;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_c6_powered_on;
static bool s_connect_handlers_registered;
static bool s_stopping;
static bool s_connected;
// Network workers may use PSRAM-backed stacks. They must never enter NVS,
// because an NVS flash read temporarily disables the external-memory cache.
// Keep the frequently queried preference in internal .data instead.
static volatile wifi_setting_network_mode_t s_network_mode = WIFI_SETTING_NETWORK_WIFI;
static volatile wifi_setting_power_mode_t s_power_mode = WIFI_SETTING_POWER_ON_DEMAND;
static volatile TaskHandle_t s_always_on_task;
static int s_retry_num;
static char s_connected_ssid[33];
static esp_netif_t *s_sta_netif;
static EventGroupHandle_t s_wifi_event_group;
static esp_event_handler_instance_t s_wifi_event_handler_instance;
static esp_event_handler_instance_t s_ip_event_handler_instance;

#if CONFIG_IDF_TARGET_ESP32S31
static bool s_native_stop_pending;
static esp_err_t wifi_setting_wait_native_stop(void);
static esp_err_t wifi_setting_suspend_native(void);
#endif

static void wifi_setting_log_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "heap %s: internal_free=%u internal_largest=%u psram_free=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void wifi_setting_connect_event_handler(void *arg,
                                               esp_event_base_t event_base,
                                               int32_t event_id,
                                               void *event_data)
{
    (void) arg;
    (void) event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SETTING_STARTED_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SETTING_STOPPED_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_stopping) {
            ESP_LOGI(TAG, "Wi-Fi disconnected during shutdown; reconnect suppressed");
            return;
        }
        if (s_retry_num < WIFI_SETTING_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying (%d/%d)", s_retry_num, WIFI_SETTING_MAX_RETRY);
        } else if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SETTING_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        if (wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_WIFI &&
            s_sta_netif != NULL) {
            (void)esp_netif_set_default_netif(s_sta_netif);
        }
        network_time_on_connected();
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SETTING_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_setting_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS");
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t wifi_setting_load_network_mode(void)
{
    uint8_t stored = WIFI_SETTING_NETWORK_WIFI;
    uint8_t stored_power = WIFI_SETTING_POWER_ON_DEMAND;
    ESP_RETURN_ON_ERROR(wifi_setting_init_nvs(), TAG,
                        "Failed to initialize NVS for network preference");

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(WIFI_SETTING_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        ret = nvs_get_u8(handle, WIFI_SETTING_NVS_NETWORK_KEY, &stored);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
            stored = WIFI_SETTING_NETWORK_WIFI;
        }
        if (ret == ESP_OK) {
            ret = nvs_get_u8(handle, WIFI_SETTING_NVS_POWER_KEY, &stored_power);
            if (ret == ESP_ERR_NVS_NOT_FOUND) {
                ret = ESP_OK;
                stored_power = WIFI_SETTING_POWER_ON_DEMAND;
            }
        }
        nvs_close(handle);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
        stored = WIFI_SETTING_NETWORK_WIFI;
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to load network preference");
    ESP_RETURN_ON_ERROR(cellular_network_load_config(), TAG, "Failed to load cellular preference");
#if CONFIG_IDF_TARGET_ESP32S31 && !CONFIG_MYPAPER_S31_CELLULAR
    stored = WIFI_SETTING_NETWORK_WIFI; // Ignore a preference from a full-feature firmware.
#endif
    s_network_mode = stored == WIFI_SETTING_NETWORK_CELLULAR
                         ? WIFI_SETTING_NETWORK_CELLULAR
                         : WIFI_SETTING_NETWORK_WIFI;
    s_power_mode = stored_power == WIFI_SETTING_POWER_ALWAYS_ON
                       ? WIFI_SETTING_POWER_ALWAYS_ON
                       : WIFI_SETTING_POWER_ON_DEMAND;
    ESP_LOGI(TAG, "Network preference loaded: %s, Wi-Fi power: %s",
             s_network_mode == WIFI_SETTING_NETWORK_CELLULAR ? "4G" : "Wi-Fi",
             s_power_mode == WIFI_SETTING_POWER_ALWAYS_ON ? "always on" : "on demand");
    return ESP_OK;
}

void wifi_setting_apply_hardware_availability(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    if (s_network_mode == WIFI_SETTING_NETWORK_CELLULAR && !cellular_network_available()) {
        // Do not erase the saved preference on a temporary detection failure.
        s_network_mode = WIFI_SETTING_NETWORK_WIFI;
        ESP_LOGW(TAG, "4G not detected at boot; using Wi-Fi for this session");
    }
#endif
}

wifi_setting_power_mode_t wifi_setting_get_power_mode(void)
{
    return s_power_mode;
}

static void wifi_setting_wait_for_always_on_task(void)
{
    // A settings scan or transport switch must not race boot-time connect.
    while (s_always_on_task != NULL &&
           s_always_on_task != xTaskGetCurrentTaskHandle()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void wifi_setting_always_on_worker(void *arg)
{
    // Let the creator publish the task handle before accessing shared state.
    vTaskDelay(pdMS_TO_TICKS(100));
    if (s_network_mode == WIFI_SETTING_NETWORK_WIFI &&
        (s_power_mode == WIFI_SETTING_POWER_ALWAYS_ON || arg != NULL)) {
        esp_err_t ret = wifi_setting_connect_saved(WIFI_SETTING_ALWAYS_ON_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Always-on Wi-Fi connect failed: %s", esp_err_to_name(ret));
        }
    }
    s_always_on_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_setting_start_always_on(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    if (s_network_mode == WIFI_SETTING_NETWORK_CELLULAR) return cellular_network_start();
#endif
    if (s_network_mode != WIFI_SETTING_NETWORK_WIFI ||
        s_power_mode != WIFI_SETTING_POWER_ALWAYS_ON ||
        s_always_on_task != NULL || wifi_setting_is_connected()) {
        return ESP_OK;
    }
    char ssid[WIFI_SETTING_SSID_MAX_LEN + 1] = {0};
    char password[WIFI_SETTING_PASSWORD_MAX_LEN + 1] = {0};
    if (!wifi_setting_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Always-on Wi-Fi waiting for saved credentials");
        return ESP_OK;
    }
    BaseType_t created = xTaskCreate(wifi_setting_always_on_worker, "wifi_always_on",
                                     4096, NULL, 4, (TaskHandle_t *)&s_always_on_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool wifi_setting_background_busy(void) { return s_always_on_task != NULL; }
esp_err_t wifi_setting_resume_saved_async(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    if (s_network_mode == WIFI_SETTING_NETWORK_CELLULAR) return cellular_network_start();
#endif
    if (s_always_on_task != NULL || wifi_setting_is_connected()) return ESP_OK;
    if (s_network_mode != WIFI_SETTING_NETWORK_WIFI) return ESP_ERR_NOT_SUPPORTED;
    BaseType_t created = xTaskCreate(wifi_setting_always_on_worker, "wifi_resume",
                                     4096, (void*)1, 4, (TaskHandle_t *)&s_always_on_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wifi_setting_set_power_mode(wifi_setting_power_mode_t mode)
{
    ESP_RETURN_ON_FALSE(mode == WIFI_SETTING_POWER_ON_DEMAND ||
                            mode == WIFI_SETTING_POWER_ALWAYS_ON,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid Wi-Fi power mode");
    ESP_RETURN_ON_ERROR(wifi_setting_init_nvs(), TAG, "Failed to initialize NVS");
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_SETTING_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "Failed to open Wi-Fi preference NVS");
    esp_err_t ret = nvs_set_u8(handle, WIFI_SETTING_NVS_POWER_KEY, (uint8_t)mode);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) return ret;
    s_power_mode = mode;
    ESP_LOGI(TAG, "Wi-Fi power mode saved: %s",
             mode == WIFI_SETTING_POWER_ALWAYS_ON ? "always on" : "on demand");
    if (s_network_mode != WIFI_SETTING_NETWORK_WIFI) return ESP_OK;
    if (mode == WIFI_SETTING_POWER_ALWAYS_ON) {
        esp_err_t start_ret = wifi_setting_start_always_on();
        if (start_ret != ESP_OK) {
            ESP_LOGW(TAG, "Always-on Wi-Fi startup deferred: %s", esp_err_to_name(start_ret));
        }
        return ESP_OK;
    }
    wifi_setting_wait_for_always_on_task();
#if CONFIG_IDF_TARGET_ESP32S31
    return wifi_setting_suspend_native();
#else
    return wifi_setting_deinit(true);
#endif
}

esp_err_t wifi_setting_release(void)
{
    // Cellular stays registered between applications. P14 also powers audio;
    // standby/network switching explicitly releases the UART data transport.
    if (s_network_mode == WIFI_SETTING_NETWORK_CELLULAR) return ESP_OK;
    if (s_network_mode == WIFI_SETTING_NETWORK_WIFI &&
        s_power_mode == WIFI_SETTING_POWER_ALWAYS_ON) {
        return ESP_OK;
    }
#if CONFIG_IDF_TARGET_ESP32S31
    return wifi_setting_suspend_native();
#else
    return wifi_setting_deinit(true);
#endif
}

#if CONFIG_IDF_TARGET_ESP32S31
static esp_err_t wifi_setting_wait_native_stop(void)
{
    if (!s_native_stop_pending) return ESP_OK;
    const EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_SETTING_STOPPED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(WIFI_SETTING_STATE_TIMEOUT_MS));
    if (!(bits & WIFI_SETTING_STOPPED_BIT)) {
        ESP_LOGE(TAG, "S31 radio suspend: STA stop event timed out");
        return ESP_ERR_TIMEOUT;
    }
    s_native_stop_pending = false;
    s_wifi_started = false;
    return ESP_OK;
}

// Match the reference board's PauseForLp/ResumeFromLp lifecycle. Native S31
// power saving stops the radio, retaining driver/netif/default RX handlers.
// Explicit wifi_setting_deinit remains available for transport handoff and
// standby; those callers really do need to destroy the driver/netif.
static esp_err_t wifi_setting_suspend_native(void)
{
    wifi_setting_wait_for_always_on_task();
    ESP_RETURN_ON_ERROR(wifi_setting_wait_native_stop(), TAG, "Previous Wi-Fi stop still pending");
    s_stopping = true; // Suppress reconnects from stop-generated events.
    s_connected = false;
    s_connected_ssid[0] = '\0';
    s_retry_num = 0;
    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_SETTING_CONNECTED_BIT |
                            WIFI_SETTING_FAIL_BIT | WIFI_SETTING_STARTED_BIT |
                            WIFI_SETTING_STOPPED_BIT);
    }
    if (s_wifi_started) {
        const esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "S31 radio suspend failed: %s", esp_err_to_name(ret));
            s_stopping = false;
            return ret;
        }
        s_native_stop_pending = true;
        ESP_RETURN_ON_ERROR(wifi_setting_wait_native_stop(), TAG, "Wi-Fi stop still pending");
    }
    s_c6_powered_on = false; // Native-board bookkeeping; there is no C6 rail.
    ESP_LOGI(TAG, "S31 Wi-Fi suspended: radio off; driver/netif retained");
    return ESP_OK;
}
#endif

wifi_setting_network_mode_t wifi_setting_get_network_mode(void)
{
    return s_network_mode;
}

esp_err_t wifi_setting_set_network_mode(wifi_setting_network_mode_t mode)
{
#if CONFIG_IDF_TARGET_ESP32S31
    ESP_RETURN_ON_FALSE(mode != WIFI_SETTING_NETWORK_CELLULAR || cellular_network_available(),
                        ESP_ERR_NOT_SUPPORTED, TAG, "4G not detected at boot");
#endif
#if CONFIG_IDF_TARGET_ESP32S31 && !CONFIG_MYPAPER_S31_CELLULAR
    ESP_RETURN_ON_FALSE(mode == WIFI_SETTING_NETWORK_WIFI, ESP_ERR_NOT_SUPPORTED,
                        TAG, "Cellular driver disabled for this hardware profile");
#endif
    ESP_RETURN_ON_FALSE(mode == WIFI_SETTING_NETWORK_WIFI ||
                            mode == WIFI_SETTING_NETWORK_CELLULAR,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid network mode");
    wifi_setting_wait_for_always_on_task();
#if CONFIG_IDF_TARGET_ESP32S31
    if (mode == WIFI_SETTING_NETWORK_CELLULAR && s_network_mode == mode)
        return cellular_network_start();
    // Complete the old transport's teardown before publishing a new route.
    // A timeout leaves the old preference intact; never destroy live DMA.
    esp_err_t handoff = mode == WIFI_SETTING_NETWORK_WIFI
        ? cellular_network_stop(15000) : wifi_setting_deinit(true);
    ESP_RETURN_ON_ERROR(handoff, TAG, "Network handoff failed");
#endif
    ESP_RETURN_ON_ERROR(wifi_setting_init_nvs(), TAG, "Failed to initialize NVS");
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_SETTING_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "Failed to open network preference NVS");
    esp_err_t ret = nvs_set_u8(handle, WIFI_SETTING_NVS_NETWORK_KEY, (uint8_t)mode);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) return ret;

    // Publish the new value only after it has been persisted successfully.
    // This is an aligned word-sized store and readers never enter NVS.
    s_network_mode = mode;

    ESP_LOGI(TAG, "Network preference saved: %s",
             mode == WIFI_SETTING_NETWORK_CELLULAR ? "4G" : "Wi-Fi");
    if (mode == WIFI_SETTING_NETWORK_CELLULAR) {
        if (s_wifi_started || s_wifi_initialized || s_c6_powered_on) {
            (void)wifi_setting_deinit(true);
        }
#if CONFIG_IDF_TARGET_ESP32S31
        return cellular_network_start();
#endif
    } else if (s_connected && s_sta_netif != NULL) {
        (void)esp_netif_set_default_netif(s_sta_netif);
    } else {
        esp_err_t start_ret = wifi_setting_start_always_on();
        if (start_ret != ESP_OK) {
            ESP_LOGW(TAG, "Always-on Wi-Fi startup deferred: %s", esp_err_to_name(start_ret));
        }
    }
    return ESP_OK;
}

esp_err_t wifi_setting_network_stack_init(void)
{
    if (!s_netif_initialized) {
        esp_err_t ret = esp_netif_init();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to initialize esp-netif: %s", esp_err_to_name(ret));
            return ret;
        }
        s_netif_initialized = true;
    }

    if (!s_event_loop_created) {
        esp_err_t ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(ret));
            return ret;
        }
        s_event_loop_created = true;
    }
    return ESP_OK;
}

static esp_err_t wifi_setting_init_events(void)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM,
                            TAG, "Failed to create Wi-Fi event group");
    }

    if (!s_connect_handlers_registered) {
        esp_err_t ret = esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            wifi_setting_connect_event_handler,
                                                            NULL,
                                                            &s_wifi_event_handler_instance);
        ESP_RETURN_ON_ERROR(ret, TAG, "Failed to register Wi-Fi event handler");
        ret = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  wifi_setting_connect_event_handler,
                                                  NULL,
                                                  &s_ip_event_handler_instance);
        if (ret != ESP_OK) {
            esp_event_handler_instance_unregister(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  s_wifi_event_handler_instance);
            s_wifi_event_handler_instance = NULL;
            ESP_RETURN_ON_ERROR(ret, TAG, "Failed to register Wi-Fi got-ip handler");
        }
        s_connect_handlers_registered = true;
    }
    return ESP_OK;
}

esp_err_t wifi_setting_c6_power_on(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    s_c6_powered_on = true;
    return ESP_OK;
#else
    if (s_c6_powered_on) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gpio_reset_pin(WIFI_SETTING_C6_EN_GPIO), TAG, "Failed to reset C6 EN GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(WIFI_SETTING_C6_EN_GPIO, 1), TAG, "Failed to enable C6");
    ESP_RETURN_ON_ERROR(gpio_set_direction(WIFI_SETTING_C6_EN_GPIO, GPIO_MODE_OUTPUT),
                        TAG,
                        "Failed to configure C6 EN GPIO");

    vTaskDelay(pdMS_TO_TICKS(WIFI_SETTING_C6_BOOT_DELAY_MS));
    s_c6_powered_on = true;
    return ESP_OK;
#endif
}

esp_err_t wifi_setting_c6_power_off(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    s_c6_powered_on = false;
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(gpio_reset_pin(WIFI_SETTING_C6_EN_GPIO), TAG, "Failed to reset C6 EN GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_direction(WIFI_SETTING_C6_EN_GPIO, GPIO_MODE_OUTPUT),
                        TAG,
                        "Failed to configure C6 EN GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(WIFI_SETTING_C6_EN_GPIO, 0), TAG, "Failed to disable C6");
    s_c6_powered_on = false;
    return ESP_OK;
#endif
}

esp_err_t wifi_setting_init(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    // Never advertise/restart STA before its previous default stop handler
    // completed, including after an earlier release timed out.
    ESP_RETURN_ON_ERROR(wifi_setting_wait_native_stop(), TAG, "Wi-Fi stop still pending");
#endif
    wifi_setting_log_heap("before-init");
    s_stopping = false;
    if (s_wifi_started) {
#if CONFIG_IDF_TARGET_ESP32S31
        return esp_wifi_set_max_tx_power(S31_WIFI_DEFAULT_TX_POWER_QDBM);
#else
        return ESP_OK;
#endif
    }

    ESP_RETURN_ON_ERROR(wifi_setting_c6_power_on(), TAG, "Failed to power on C6");
    ESP_RETURN_ON_ERROR(wifi_setting_init_nvs(), TAG, "Failed to initialize NVS");

    ESP_RETURN_ON_ERROR(wifi_setting_network_stack_init(), TAG,
                        "Failed to initialize shared network stack");

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG, "Failed to create default Wi-Fi STA netif");
    }

    // Register after the default netif handlers so STARTED/STOPPED bits mean
    // lwIP has already completed the corresponding add/remove operation.
    ESP_RETURN_ON_ERROR(wifi_setting_init_events(), TAG, "Failed to initialize Wi-Fi events");

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32S31
        wifi_setting_log_heap("driver-init-begin");
        ESP_LOGI(TAG, "RX profile: static=%u dynamic=%u ba=%u tcp_window=%u tcp_mailbox=%u tcpip_mailbox=%u",
                 (unsigned)cfg.static_rx_buf_num, (unsigned)cfg.dynamic_rx_buf_num, (unsigned)cfg.rx_ba_win,
                 (unsigned)CONFIG_LWIP_TCP_WND_DEFAULT, (unsigned)CONFIG_LWIP_TCP_RECVMBOX_SIZE,
                 (unsigned)CONFIG_LWIP_TCPIP_RECVMBOX_SIZE);
#endif
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Failed to initialize Wi-Fi");
#if CONFIG_IDF_TARGET_ESP32S31
        wifi_setting_log_heap("driver-init-done");
#endif
        s_wifi_initialized = true;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Failed to set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set Wi-Fi STA mode");
#if CONFIG_IDF_TARGET_ESP32S31
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocol(WIFI_IF_STA, S31_WIFI_DEFAULT_PROTOCOL),
                        TAG, "Failed to set S31 Wi-Fi protocol");
    ESP_LOGI(TAG, "S31 Wi-Fi STA protocol: 802.11b/g/n/ax");
#endif
    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group,
                             WIFI_SETTING_STARTED_BIT | WIFI_SETTING_STOPPED_BIT);
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");
    s_wifi_started = true;
#if CONFIG_IDF_TARGET_ESP32S31
    ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(S31_WIFI_DEFAULT_TX_POWER_QDBM),
                        TAG, "Failed to set S31 Wi-Fi 5 dBm limit");
    ESP_LOGI(TAG, "S31 Wi-Fi radio started with reusable driver/netif");
#endif

    if (s_wifi_event_group != NULL) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_SETTING_STARTED_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               pdMS_TO_TICKS(WIFI_SETTING_STATE_TIMEOUT_MS));
        if (!(bits & WIFI_SETTING_STARTED_BIT)) {
            ESP_LOGW(TAG, "Timed out waiting for Wi-Fi STA start event");
        }
    }

    /*
     * Keep the official station example flow: the hosted remote Wi-Fi layer
     * handles the co-processor power-save details.
     */
    // ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Failed to disable Wi-Fi power save");
    wifi_setting_log_heap("after-start");
    return ESP_OK;
}

esp_err_t wifi_setting_scan(wifi_setting_ap_t *records,
                            size_t capacity,
                            size_t *out_count)
{
    wifi_setting_wait_for_always_on_task();
    ESP_RETURN_ON_FALSE(out_count != NULL, ESP_ERR_INVALID_ARG, TAG, "Missing scan count output");
    ESP_RETURN_ON_FALSE(capacity == 0 || records != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "Missing scan record buffer");
    *out_count = 0;
    ESP_RETURN_ON_ERROR(wifi_setting_init(), TAG, "Failed to initialize Wi-Fi");

    ESP_LOGI(TAG, "Starting Wi-Fi scan");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_config, true), TAG, "Wi-Fi scan failed");

    uint16_t ap_count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_count), TAG, "Failed to get AP count");

    wifi_ap_record_t ap_records[WIFI_SETTING_MAX_AP_COUNT];
    uint16_t record_count = ap_count;
    if (record_count > WIFI_SETTING_MAX_AP_COUNT) {
        record_count = WIFI_SETTING_MAX_AP_COUNT;
    }

    memset(ap_records, 0, sizeof(ap_records));
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&record_count, ap_records),
                        TAG, "Failed to get AP records");

    ESP_LOGI(TAG, "Wi-Fi scan done, found %u APs, showing %u", ap_count, record_count);
    for (uint16_t i = 0; i < record_count; ++i) {
        ESP_LOGI(TAG, "%2u: rssi=%4d channel=%2u auth=%d ssid=%s",
                 i + 1,
                 ap_records[i].rssi,
                 ap_records[i].primary,
                 ap_records[i].authmode,
                 (const char *) ap_records[i].ssid);
        if (*out_count < capacity && ap_records[i].ssid[0] != '\0') {
            wifi_setting_ap_t *output = &records[*out_count];
            memset(output, 0, sizeof(*output));
            strlcpy(output->ssid, (const char *) ap_records[i].ssid, sizeof(output->ssid));
            output->rssi = ap_records[i].rssi;
            output->channel = ap_records[i].primary;
            output->secured = ap_records[i].authmode != WIFI_AUTH_OPEN;
            (*out_count)++;
        }
    }

    return ESP_OK;
}

esp_err_t wifi_setting_connect(const char *ssid, const char *password, int timeout_ms)
{
    wifi_setting_wait_for_always_on_task();
    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "Wi-Fi SSID is empty");
    ESP_RETURN_ON_FALSE(strlen(ssid) <= WIFI_SETTING_SSID_MAX_LEN, ESP_ERR_INVALID_ARG,
                        TAG, "Wi-Fi SSID is too long");
    ESP_RETURN_ON_FALSE(password == NULL || strlen(password) <= WIFI_SETTING_PASSWORD_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "Wi-Fi password is too long");
    ESP_RETURN_ON_ERROR(wifi_setting_init(), TAG, "Failed to initialize Wi-Fi");

    esp_netif_ip_info_t ip_info = {0};
    if (s_connected && strcmp(s_connected_ssid, ssid) == 0 &&
        s_sta_netif != NULL && esp_netif_is_netif_up(s_sta_netif) &&
        esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        ESP_LOGI(TAG, "Reusing Wi-Fi connection SSID=%s IP=" IPSTR,
                 ssid, IP2STR(&ip_info.ip));
        return ESP_OK;
    }

    wifi_config_t wifi_config = {0};
    size_t ssid_length = strlen(ssid);
    memcpy(wifi_config.sta.ssid, ssid,
           ssid_length < sizeof(wifi_config.sta.ssid)
               ? ssid_length
               : sizeof(wifi_config.sta.ssid));
    if (password != NULL) {
        strlcpy((char *) wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }
    // Let the selected AP negotiate its advertised security mode. Requiring
    // WPA2 here incorrectly rejects open, WPA and mixed-mode networks.
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    xEventGroupClearBits(s_wifi_event_group, WIFI_SETTING_CONNECTED_BIT | WIFI_SETTING_FAIL_BIT);
    s_retry_num = 0;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "Failed to set Wi-Fi config");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Failed to start Wi-Fi connection");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_SETTING_CONNECTED_BIT | WIFI_SETTING_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_SETTING_CONNECTED_BIT) {
        strlcpy(s_connected_ssid, ssid, sizeof(s_connected_ssid));
        ESP_LOGI(TAG, "Connected to Wi-Fi SSID: %s", ssid);
        return ESP_OK;
    }

    if (bits & WIFI_SETTING_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi SSID: %s", ssid);
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "Timed out connecting to Wi-Fi SSID: %s", ssid);
    return ESP_ERR_TIMEOUT;
}

bool wifi_setting_load_credentials(char *ssid,
                                   size_t ssid_size,
                                   char *password,
                                   size_t password_size)
{
    if (ssid == NULL || ssid_size == 0 || password == NULL || password_size == 0) {
        return false;
    }
    ssid[0] = '\0';
    password[0] = '\0';
    if (wifi_setting_init_nvs() != ESP_OK) return false;

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(WIFI_SETTING_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) return false;

    size_t stored_ssid_size = ssid_size;
    ret = nvs_get_str(handle, WIFI_SETTING_NVS_SSID_KEY, ssid, &stored_ssid_size);
    if (ret == ESP_OK) {
        size_t stored_password_size = password_size;
        esp_err_t pass_ret = nvs_get_str(handle,
                                         WIFI_SETTING_NVS_PASSWORD_KEY,
                                         password,
                                         &stored_password_size);
        if (pass_ret == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
        } else if (pass_ret != ESP_OK) {
            ret = pass_ret;
        }
    }
    nvs_close(handle);

    if (ret != ESP_OK || ssid[0] == '\0') {
        ssid[0] = '\0';
        password[0] = '\0';
        return false;
    }
    return true;
}

esp_err_t wifi_setting_save_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG,
                        TAG, "Wi-Fi SSID is empty");
    ESP_RETURN_ON_FALSE(strlen(ssid) <= WIFI_SETTING_SSID_MAX_LEN, ESP_ERR_INVALID_ARG,
                        TAG, "Wi-Fi SSID is too long");
    ESP_RETURN_ON_FALSE(password == NULL || strlen(password) <= WIFI_SETTING_PASSWORD_MAX_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "Wi-Fi password is too long");
    ESP_RETURN_ON_ERROR(wifi_setting_init_nvs(), TAG, "Failed to initialize NVS");

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_SETTING_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "Failed to open Wi-Fi NVS namespace");
    esp_err_t ret = nvs_set_str(handle, WIFI_SETTING_NVS_SSID_KEY, ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, WIFI_SETTING_NVS_PASSWORD_KEY,
                          password != NULL ? password : "");
    }
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Saved default Wi-Fi SSID: %s", ssid);
    return ret;
}

esp_err_t wifi_setting_clear_credentials(void)
{
    ESP_RETURN_ON_ERROR(wifi_setting_init_nvs(), TAG, "Failed to initialize NVS");
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_SETTING_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "Failed to open Wi-Fi NVS namespace");
    // A Wi-Fi credential reset must not silently reset the user's choice of
    // Wi-Fi versus cellular data.
    esp_err_t ret = nvs_erase_key(handle, WIFI_SETTING_NVS_SSID_KEY);
    if (ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = nvs_erase_key(handle, WIFI_SETTING_NVS_PASSWORD_KEY);
    }
    if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

bool wifi_setting_is_enabled(void)
{
    return s_wifi_started && !s_stopping;
}

bool wifi_setting_is_connected(void)
{
    if (wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR)
        return cellular_network_is_online();
    esp_netif_ip_info_t ip_info = {0};
    const bool wifi_online = !s_stopping && s_connected && s_sta_netif != NULL &&
                             esp_netif_is_netif_up(s_sta_netif) &&
                             esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK &&
                             ip_info.ip.addr != 0;
    return wifi_online;
}

esp_err_t wifi_setting_connect_saved(int timeout_ms)
{
    if (wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR) {
        ESP_LOGI(TAG, "Using selected cellular data connection");
        return cellular_network_wait_online(timeout_ms);
    }
    char ssid[WIFI_SETTING_SSID_MAX_LEN + 1] = {0};
    char password[WIFI_SETTING_PASSWORD_MAX_LEN + 1] = {0};
    ESP_RETURN_ON_FALSE(wifi_setting_load_credentials(ssid, sizeof(ssid),
                                                      password, sizeof(password)),
                        ESP_ERR_NOT_FOUND, TAG, "No saved Wi-Fi credentials");
    return wifi_setting_connect(ssid, password, timeout_ms);
}

esp_err_t wifi_setting_deinit(bool power_down_c6)
{
    wifi_setting_wait_for_always_on_task();
#if CONFIG_IDF_TARGET_ESP32S31
    ESP_RETURN_ON_ERROR(wifi_setting_wait_native_stop(), TAG, "Wi-Fi stop still pending");
#endif
    wifi_setting_log_heap("before-deinit");
    esp_err_t first_error = ESP_OK;
    s_stopping = true;

    if (s_wifi_started) {
        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_SETTING_STOPPED_BIT);
        }
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop Wi-Fi: %s", esp_err_to_name(ret));
            first_error = ret;
            if (!power_down_c6) {
                return ret;
            }
            if (s_sta_netif != NULL) {
                esp_netif_action_stop(s_sta_netif, NULL, 0, NULL);
            }
        } else if (s_wifi_event_group != NULL) {
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_SETTING_STOPPED_BIT,
                                                   pdFALSE,
                                                   pdTRUE,
                                                   pdMS_TO_TICKS(WIFI_SETTING_STATE_TIMEOUT_MS));
            if (!(bits & WIFI_SETTING_STOPPED_BIT)) {
                ESP_LOGW(TAG, "Timed out waiting for Wi-Fi STA stop event; forcing netif stop");
                first_error = ESP_ERR_TIMEOUT;
                if (s_sta_netif != NULL) {
                    esp_netif_action_stop(s_sta_netif, NULL, 0, NULL);
                }
            }
        }
        s_wifi_started = false;
    }

    if (s_wifi_initialized) {
        esp_err_t ret = esp_wifi_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to deinitialize Wi-Fi: %s", esp_err_to_name(ret));
            if (first_error == ESP_OK) {
                first_error = ret;
            }
            if (!power_down_c6) {
                return ret;
            }
        }
        s_wifi_initialized = false;
    }

    s_connected = false;
    s_connected_ssid[0] = '\0';

    if (power_down_c6) {
#if CONFIG_IDF_TARGET_ESP32P4
        esp_err_t hosted_ret = esp_hosted_prepare_slave_power_down();
        if (hosted_ret != ESP_OK && first_error == ESP_OK) {
            first_error = hosted_ret;
        }
#endif
        esp_err_t ret = wifi_setting_c6_power_off();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        } else if (ret == ESP_OK) {
            /*
             * ESP-Hosted delivers Wi-Fi events from the C6 asynchronously.
             * Keep the slave powered down long enough for queued events to be
             * discarded before another activity is allowed to boot it again.
             */
#if CONFIG_IDF_TARGET_ESP32P4
            vTaskDelay(pdMS_TO_TICKS(WIFI_SETTING_C6_OFF_SETTLE_MS));
#endif
        }
    }

    if (s_connect_handlers_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              s_wifi_event_handler_instance);
        esp_event_handler_instance_unregister(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              s_ip_event_handler_instance);
        s_wifi_event_handler_instance = NULL;
        s_ip_event_handler_instance = NULL;
        s_connect_handlers_registered = false;
    }

    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (power_down_c6) {
        ESP_LOGI(TAG, "Wi-Fi fully shut down; C6 power is off");
    }

    wifi_setting_log_heap("after-deinit");
    return first_error;
}

esp_err_t wifi_setting_reset_driver_config(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    const esp_err_t cellular_result = cellular_network_suspend();
    if (cellular_result != ESP_OK) return cellular_result;
#endif
    const wifi_setting_power_mode_t previous_power = s_power_mode;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result;
    esp_err_t cleanup_result;
    // Prevent the ordinary always-on path from starting another connection
    // while the confirmed reset clears persistent settings.
    s_power_mode = WIFI_SETTING_POWER_ON_DEMAND;
    result = wifi_setting_deinit(true); // Joins the existing always-on worker.
    if (result != ESP_OK) {
        s_power_mode = previous_power;
        return result;
    }
    // No full-partition erase on NVS errors: login/calibration data is kept.
    result = nvs_flash_init();
    if (result != ESP_OK) goto cleanup;
    result = wifi_setting_network_stack_init();
    if (result != ESP_OK) goto cleanup;
    result = wifi_setting_c6_power_on();
    if (result != ESP_OK) goto cleanup;
    // Initialize only the driver. Do not start STA, connect with old config,
    // create a station netif or trigger boot-time network time synchronization.
    result = esp_wifi_init(&cfg);
    if (result != ESP_OK) goto cleanup;
    s_wifi_initialized = true;
    result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (result == ESP_OK) result = esp_wifi_restore();
    if (result == ESP_OK) result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
cleanup:
    cleanup_result = wifi_setting_deinit(true);
    if (result == ESP_OK) result = cleanup_result;
    if (result != ESP_OK) {
        s_power_mode = previous_power;
        ESP_LOGE(TAG, "Wi-Fi driver defaults reset failed: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(TAG, "Wi-Fi driver defaults restored; radio off");
    }
    return result;
}
