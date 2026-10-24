#include "storage/settings_reset.h"
#include "storage/app_paths.h"
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
}

esp_err_t settings_restore_defaults()
{
    // Never erase the whole NVS partition. Stock watchlists and WeRead login
    // are user data, rather than system preferences.
    static constexpr const char *names[] = {
        "wifi_cfg", "frontlight", "frontlight_s31", "home_ui", "time_basis", "ds_chat", "amap",
        "reader_style", "rss_reader", "reader_bg", "rss_reader_bg",
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
    for (const char *name : names) {
        const esp_err_t result = clear_preferences(name);
        if (result != ESP_OK) return result;
    }
    return clear_preferences("phone", "ringtone");
}
