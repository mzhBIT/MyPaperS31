#include "map/AmapConfig.h"

#include <nvs.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr const char *NVS_NAMESPACE = "amap";
constexpr const char *KEY_API_KEY = "api_key";
constexpr const char *KEY_LONGITUDE = "last_lon";
constexpr const char *KEY_LATITUDE = "last_lat";
constexpr const char *KEY_ZOOM = "last_zoom";

int32_t degrees_to_e6(double value)
{
    return static_cast<int32_t>(std::llround(value * 1000000.0));
}

}

bool amap_config_load(AmapConfig *config)
{
    if (!config) return false;
    *config = AmapConfig{};
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t key_size = 0;
    if (nvs_get_str(handle, KEY_API_KEY, nullptr, &key_size) == ESP_OK &&
        key_size > 1 && key_size <= 256) {
        std::vector<char> value(key_size);
        if (nvs_get_str(handle, KEY_API_KEY, value.data(), &key_size) == ESP_OK) {
            config->api_key.assign(value.data());
        }
    }

    int32_t coordinate = 0;
    if (nvs_get_i32(handle, KEY_LONGITUDE, &coordinate) == ESP_OK) {
        config->longitude = static_cast<double>(coordinate) / 1000000.0;
    }
    if (nvs_get_i32(handle, KEY_LATITUDE, &coordinate) == ESP_OK) {
        config->latitude = static_cast<double>(coordinate) / 1000000.0;
    }
    uint8_t zoom = config->zoom;
    if (nvs_get_u8(handle, KEY_ZOOM, &zoom) == ESP_OK) {
        config->zoom = std::max<uint8_t>(1, std::min<uint8_t>(17, zoom));
    }
    nvs_close(handle);
    return !config->api_key.empty();
}

bool amap_config_save_api_key(const std::string &api_key)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = api_key.empty() ? nvs_erase_key(handle, KEY_API_KEY)
                                      : nvs_set_str(handle, KEY_API_KEY, api_key.c_str());
    if (error == ESP_ERR_NVS_NOT_FOUND && api_key.empty()) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

bool amap_config_save_viewport(double longitude, double latitude, uint8_t zoom)
{
    if (!std::isfinite(longitude) || !std::isfinite(latitude)) return false;
    longitude = std::max(-180.0, std::min(180.0, longitude));
    latitude = std::max(-85.051129, std::min(85.051129, latitude));
    zoom = std::max<uint8_t>(1, std::min<uint8_t>(17, zoom));

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = nvs_set_i32(handle, KEY_LONGITUDE, degrees_to_e6(longitude));
    if (error == ESP_OK) error = nvs_set_i32(handle, KEY_LATITUDE, degrees_to_e6(latitude));
    if (error == ESP_OK) error = nvs_set_u8(handle, KEY_ZOOM, zoom);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}
