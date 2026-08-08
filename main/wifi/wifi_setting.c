#include "wifi_setting.h"

#include <stdbool.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define WIFI_SETTING_C6_EN_GPIO        GPIO_NUM_54
#define WIFI_SETTING_C6_BOOT_DELAY_MS  1000
#define WIFI_SETTING_MAX_AP_COUNT      16
#define WIFI_SETTING_CONNECTED_BIT     BIT0
#define WIFI_SETTING_FAIL_BIT          BIT1
#define WIFI_SETTING_MAX_RETRY         5

static const char *TAG = "wifi_setting";

static bool s_netif_initialized;
static bool s_event_loop_created;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_c6_powered_on;
static bool s_connect_handlers_registered;
static int s_retry_num;
static esp_netif_t *s_sta_netif;
static EventGroupHandle_t s_wifi_event_group;

static void wifi_setting_connect_event_handler(void *arg,
                                               esp_event_base_t event_base,
                                               int32_t event_id,
                                               void *event_data)
{
    (void) arg;
    (void) event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
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

esp_err_t wifi_setting_c6_power_on(void)
{
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
}

esp_err_t wifi_setting_c6_power_off(void)
{
    ESP_RETURN_ON_ERROR(gpio_reset_pin(WIFI_SETTING_C6_EN_GPIO), TAG, "Failed to reset C6 EN GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_direction(WIFI_SETTING_C6_EN_GPIO, GPIO_MODE_OUTPUT),
                        TAG,
                        "Failed to configure C6 EN GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(WIFI_SETTING_C6_EN_GPIO, 0), TAG, "Failed to disable C6");
    s_c6_powered_on = false;
    return ESP_OK;
}

esp_err_t wifi_setting_init(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(wifi_setting_c6_power_on(), TAG, "Failed to power on C6");
    ESP_RETURN_ON_ERROR(wifi_setting_init_nvs(), TAG, "Failed to initialize NVS");

    if (!s_netif_initialized) {
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Failed to initialize esp-netif");
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

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG, "Failed to create default Wi-Fi STA netif");
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Failed to initialize Wi-Fi");
        s_wifi_initialized = true;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Failed to set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set Wi-Fi STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");
    s_wifi_started = true;

    /*
     * Keep the official station example flow: the hosted remote Wi-Fi layer
     * handles the co-processor power-save details.
     */
    // ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Failed to disable Wi-Fi power save");
    return ESP_OK;
}

esp_err_t wifi_setting_scan(void)
{
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
    }

    return ESP_OK;
}

esp_err_t wifi_setting_connect(const char *ssid, const char *password, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "Wi-Fi SSID is empty");
    ESP_RETURN_ON_ERROR(wifi_setting_init(), TAG, "Failed to initialize Wi-Fi");

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create Wi-Fi event group");
    }

    if (!s_connect_handlers_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                               WIFI_EVENT_STA_DISCONNECTED,
                                                               wifi_setting_connect_event_handler,
                                                               NULL,
                                                               NULL),
                            TAG,
                            "Failed to register Wi-Fi disconnect handler");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                               IP_EVENT_STA_GOT_IP,
                                                               wifi_setting_connect_event_handler,
                                                               NULL,
                                                               NULL),
                            TAG,
                            "Failed to register Wi-Fi got-ip handler");
        s_connect_handlers_registered = true;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password != NULL) {
        strlcpy((char *) wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (password == NULL || password[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

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

esp_err_t wifi_setting_deinit(bool power_down_c6)
{
    esp_err_t first_error = ESP_OK;

    if (s_wifi_started) {
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        s_wifi_started = false;
    }

    if (s_wifi_initialized) {
        esp_err_t ret = esp_wifi_deinit();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        s_wifi_initialized = false;
    }

    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (power_down_c6) {
        esp_err_t ret = wifi_setting_c6_power_off();
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }

    return first_error;
}
