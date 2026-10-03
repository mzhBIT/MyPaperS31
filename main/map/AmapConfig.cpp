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

    uint8_t source = 0;
    nvs_get_u8(handle, "source", &source);
    config->offline = source == 1;
    size_t path_size = 0;
    if (nvs_get_str(handle, "offline_path", nullptr, &path_size) == ESP_OK && path_size > 1 && path_size <= 256) {
        std::vector<char> path(path_size);
        if (nvs_get_str(handle, "offline_path", path.data(), &path_size) == ESP_OK) config->offline_path = path.data();
    }
    const char *lon_key = config->offline ? "off_lon" : KEY_LONGITUDE;
    const char *lat_key = config->offline ? "off_lat" : KEY_LATITUDE;
    const char *zoom_key = config->offline ? "off_zoom" : KEY_ZOOM;
    int32_t coordinate = 0;
    const bool have_lon = nvs_get_i32(handle, lon_key, &coordinate) == ESP_OK;
    if (have_lon) {
        config->longitude = static_cast<double>(coordinate) / 1000000.0;
    }
    const bool have_lat = nvs_get_i32(handle, lat_key, &coordinate) == ESP_OK;
    if (have_lat) {
        config->latitude = static_cast<double>(coordinate) / 1000000.0;
    }
    uint8_t zoom = config->zoom;
    const bool have_zoom = nvs_get_u8(handle, zoom_key, &zoom) == ESP_OK;
    if (have_zoom) {
        config->zoom = std::max<uint8_t>(config->offline ? 0 : 1, std::min<uint8_t>(config->offline ? 22 : 17, zoom));
    }
    config->offline_viewport_saved = config->offline && have_lon && have_lat && have_zoom;
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

bool amap_config_save_viewport(double longitude, double latitude, uint8_t zoom, bool offline)
{
    if (!std::isfinite(longitude) || !std::isfinite(latitude)) return false;
    longitude = std::max(-180.0, std::min(180.0, longitude));
    latitude = std::max(-85.051129, std::min(85.051129, latitude));
    zoom = std::max<uint8_t>(offline ? 0 : 1, std::min<uint8_t>(offline ? 22 : 17, zoom));

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = nvs_set_i32(handle, offline ? "off_lon" : KEY_LONGITUDE, degrees_to_e6(longitude));
    if (error == ESP_OK) error = nvs_set_i32(handle, offline ? "off_lat" : KEY_LATITUDE, degrees_to_e6(latitude));
    if (error == ESP_OK) error = nvs_set_u8(handle, offline ? "off_zoom" : KEY_ZOOM, zoom);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

bool amap_config_save_source(bool offline)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, "source", offline ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool amap_config_save_offline_path(const std::string &path)
{
    if (path.size() > 255 || path.rfind("/sdcard/", 0) != 0 || path.find("/../") != std::string::npos) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, "offline_path", path.c_str());
    for (const char *key : {"off_lon", "off_lat", "off_zoom"}) {
        if (e != ESP_OK) break;
        esp_err_t erase = nvs_erase_key(h, key);
        if (erase != ESP_OK && erase != ESP_ERR_NVS_NOT_FOUND) e = erase;
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
