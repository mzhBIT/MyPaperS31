#include "zhihu/ZhihuConfig.h"

#include <nvs.h>

namespace {

constexpr const char *NVS_NAMESPACE = "zhihu";
constexpr const char *NVS_ACCESS_SECRET = "access_secret";
constexpr size_t MAX_ACCESS_SECRET_BYTES = 255;

}  // namespace

bool zhihu_config_load(ZhihuConfig *config)
{
    if (!config) return false;
    *config = {};

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = 0;
    if (nvs_get_str(handle, NVS_ACCESS_SECRET, nullptr, &size) == ESP_OK &&
        size > 1 && size <= MAX_ACCESS_SECRET_BYTES + 1) {
        std::string value(size, '\0');
        if (nvs_get_str(handle, NVS_ACCESS_SECRET, value.data(), &size) == ESP_OK) {
            value.resize(size - 1);
            config->access_secret = std::move(value);
        }
    }
    nvs_close(handle);
    return !config->access_secret.empty();
}

bool zhihu_config_save_access_secret(const std::string &access_secret)
{
    if (access_secret.size() > MAX_ACCESS_SECRET_BYTES) return false;
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = access_secret.empty()
                          ? nvs_erase_key(handle, NVS_ACCESS_SECRET)
                          : nvs_set_str(handle, NVS_ACCESS_SECRET, access_secret.c_str());
    if (error == ESP_ERR_NVS_NOT_FOUND && access_secret.empty()) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

bool zhihu_config_has_access_secret()
{
    ZhihuConfig config;
    return zhihu_config_load(&config);
}
