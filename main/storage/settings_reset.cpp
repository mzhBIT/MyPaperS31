#include "storage/settings_reset.h"
#include "storage/app_paths.h"
#include "wifi/wifi_setting.h"
#include <nvs.h>
#include <esp_log.h>
#include <cerrno>
#include <cstdio>

namespace {
constexpr const char *TAG = "SETTINGS_RESET";
esp_err_t clear_preferences(const char *name, const char *key = nullptr)
{
    nvs_handle_t handle;
    // Skip absent namespaces without consuming NVS space.
    esp_err_t result = nvs_open(name, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    nvs_close(handle);
    result = nvs_open(name, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = key ? nvs_erase_key(handle, key) : nvs_erase_all(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) ESP_LOGE(TAG, "reset %s failed: %s", name, esp_err_to_name(result));
    return result;
}

esp_err_t verify_key_erased(const char *name, const char *key)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(name, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    size_t size = 0;
    result = nvs_get_str(handle, key, nullptr, &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_LOGE(TAG, "reset verification failed: namespace=%s key=%s result=%s", name, key,
             esp_err_to_name(result));
    return result == ESP_OK ? ESP_FAIL : result;
}

esp_err_t verify_credentials_erased()
{
    static constexpr const char *keys[][2] = {
        {"wifi_cfg", "ssid"}, {"wifi_cfg", "password"},
        {"ds_chat", "api_key"}, {"amap", "api_key"},
    };
    for (const auto &entry : keys) {
        const esp_err_t result = verify_key_erased(entry[0], entry[1]);
        if (result != ESP_OK) return result;
    }
    ESP_LOGI(TAG, "reset verified: saved Wi-Fi credentials and DeepSeek/AMap API keys absent");
    return ESP_OK;
}
}

esp_err_t settings_restore_defaults()
{
    // Never erase the whole NVS partition. Stock watchlists and WeRead login
    // are user data, rather than system preferences.
    static constexpr const char *names[] = {
        "wifi_cfg", "cellular_s31", "frontlight", "frontlight_s31", "home_ui", "time_basis", "ds_chat", "amap",
        "reader_style", "rss_reader", "reader_bg", "rss_reader_bg", "standby_bg",
        "reader_cover", "rss_reader_cover", "script_mode", "writing_mode",
        "audio_out", "text_input", "weread_pref", "system_ui", "system_font", "bookshelf_ui"
    };
    const char *files[] = {app_paths::reader_style_file(), app_paths::reader_font_file()};
    // Remove old SD fallbacks too, so they cannot resurrect prior preferences.
    for (const char *path : files) {
        if (std::remove(path) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "remove preference %s failed: errno=%d", path, errno);
            return ESP_FAIL;
        }
    }
    // wifi_cfg belongs to this application, while the SDK can retain older
    // station config separately. Shut down Wi-Fi and clear both layers.
    const esp_err_t wifi_result = wifi_setting_reset_driver_config();
    if (wifi_result != ESP_OK) return wifi_result;
    for (const char *name : names) {
        const esp_err_t result = clear_preferences(name);
        if (result != ESP_OK) return result;
    }
    const esp_err_t ringtone_result = clear_preferences("phone", "ringtone");
    if (ringtone_result != ESP_OK) return ringtone_result;
    return verify_credentials_erased();
}
