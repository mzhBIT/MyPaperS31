#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "audio/audio_radio.h"
#include "storage/storage_sdcard.h"
#include "wifi/wifi_setting.h"

#define AUDIO_RECORD_TEST_PATH        "/sdcard/record.wav"
#define AUDIO_RECORD_TEST_URI         "file://sdcard/record.wav"
#define AUDIO_RECORD_TEST_DURATION_MS 10000
#define AUDIO_RECORD_TEST_PLAY_MS     12000

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t ret = storage_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SD card mounted at %s", storage_sdcard_get_mount_point());

        ESP_LOGI(TAG, "Temporary audio test: record %d ms to %s",
                 AUDIO_RECORD_TEST_DURATION_MS, AUDIO_RECORD_TEST_PATH);
        ret = audio_radio_record_wav(AUDIO_RECORD_TEST_PATH, AUDIO_RECORD_TEST_DURATION_MS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio record test failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Temporary audio test: play %s", AUDIO_RECORD_TEST_URI);
            ret = audio_radio_play(AUDIO_RECORD_TEST_URI);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Audio record playback failed: %s", esp_err_to_name(ret));
            } else {
                vTaskDelay(pdMS_TO_TICKS(AUDIO_RECORD_TEST_PLAY_MS));
                ret = audio_radio_stop();
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Audio record playback stop failed: %s", esp_err_to_name(ret));
                }
            }
        }
    }

    if (CONFIG_WIFI_SETTING_STA_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty; scanning APs and skipping radio playback");
        ret = wifi_setting_scan();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(ret));
        }
        return;
    }

    ret = wifi_setting_connect(CONFIG_WIFI_SETTING_STA_SSID,
                               CONFIG_WIFI_SETTING_STA_PASSWORD,
                               CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(ret));
        return;
    }

#if CONFIG_AUDIO_RADIO_ENABLE_ON_BOOT
    ret = audio_radio_play_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio radio start failed: %s", esp_err_to_name(ret));
        return;
    }
#endif
}
