#include "time/network_time.h"

#include <atomic>
#include <ctime>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "NETWORK_TIME";
std::atomic<bool> s_sync_busy{false};
std::atomic<bool> s_boot_attempt_started{false};

void boot_sync_task(void *)
{
    ESP_LOGI(TAG, "boot automatic sync started");
    const esp_err_t ret = network_time_sync(10000);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "boot automatic sync complete unix=%lld",
                 static_cast<long long>(std::time(nullptr)));
    } else {
        ESP_LOGW(TAG, "boot automatic sync failed: %s; no automatic retry",
                 esp_err_to_name(ret));
    }
    vTaskDelete(nullptr);
}
}

extern "C" esp_err_t network_time_sync(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    bool expected = false;
    while (!s_sync_busy.compare_exchange_weak(expected, true)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        expected = false;
        vTaskDelay(1);
    }
    esp_err_t ret = ESP_ERR_TIMEOUT;
    const int64_t remaining_us = deadline - esp_timer_get_time();
    if (remaining_us > 0) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        ret = esp_netif_sntp_init(&config);
        if (ret == ESP_OK) {
            TickType_t wait_ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
            if (wait_ticks == 0) wait_ticks = 1;
            ret = esp_netif_sntp_sync_wait(wait_ticks);
            esp_netif_sntp_deinit(); // No periodic synchronization after this request.
            if (ret == ESP_OK && std::time(nullptr) < 1704067200) {
                ret = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    s_sync_busy.store(false);
    return ret;
}

extern "C" void network_time_on_connected(void)
{
    bool expected = false;
    if (!s_boot_attempt_started.compare_exchange_strong(expected, true)) return;
    // Default FreeRTOS task creation uses an internal-RAM stack. Keep this
    // short-lived task on core 0, away from the EPD scan task on core 1.
    if (xTaskCreatePinnedToCore(boot_sync_task, "boot_time_sync", 3072,
                                nullptr, 1, nullptr, 0) != pdPASS) {
        s_boot_attempt_started.store(false); // Allow a later IP event to create it.
        ESP_LOGW(TAG, "boot sync task allocation failed");
    }
}
