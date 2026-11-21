#pragma once

#include <sdkconfig.h>

#if CONFIG_IDF_TARGET_ESP32S31
#include <esp_log.h>
#include <esp_wifi.h>

#include <mutex>
#endif

namespace wifi {

class RequestPowerGuard {
public:
    explicit RequestPowerGuard(const char *tag, bool enabled = true)
    {
#if CONFIG_IDF_TARGET_ESP32S31
        if (!enabled) return;
        std::lock_guard<std::mutex> lock(_mutex);
        if (_users == 0) {
            const auto read = esp_wifi_get_ps(&_previous);
            if (read != ESP_OK) {
                ESP_LOGW(tag, "request Wi-Fi power mode read failed: %s", esp_err_to_name(read));
                return;
            }
            if (_previous != WIFI_PS_NONE) {
                const auto result = esp_wifi_set_ps(WIFI_PS_NONE);
                ESP_LOGI(tag, "request Wi-Fi wake: previous_ps=%d applied=%d",
                         static_cast<int>(_previous), result == ESP_OK);
                if (result != ESP_OK) return;
            }
        }
        ++_users;
        _tag = tag;
#else
        (void)tag;
        (void)enabled;
#endif
    }

    ~RequestPowerGuard()
    {
#if CONFIG_IDF_TARGET_ESP32S31
        if (!_tag) return;
        std::lock_guard<std::mutex> lock(_mutex);
        if (--_users == 0 && _previous != WIFI_PS_NONE) {
            const auto result = esp_wifi_set_ps(_previous);
            ESP_LOGI(_tag, "request Wi-Fi restore: ps=%d result=%s",
                     static_cast<int>(_previous), esp_err_to_name(result));
        }
#endif
    }

    RequestPowerGuard(const RequestPowerGuard &) = delete;
    RequestPowerGuard &operator=(const RequestPowerGuard &) = delete;

private:
#if CONFIG_IDF_TARGET_ESP32S31
    inline static std::mutex _mutex;
    inline static unsigned _users = 0;
    inline static wifi_ps_type_t _previous = WIFI_PS_NONE;
    const char *_tag = nullptr;
#endif
};

}  // namespace wifi
