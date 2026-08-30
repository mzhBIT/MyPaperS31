#include "deepseek/DeepSeekConfig.h"

#include <nvs.h>

namespace {

constexpr const char *NVS_NAMESPACE = "ds_chat";
constexpr const char *NVS_API_KEY = "api_key";
constexpr const char *NVS_MODEL = "model";
constexpr size_t MAX_API_KEY_BYTES = 255;

}  // namespace

bool deepseek_config_load(DeepSeekConfig *config)
{
    if (!config) return false;
    *config = {};

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t key_size = 0;
    if (nvs_get_str(handle, NVS_API_KEY, nullptr, &key_size) == ESP_OK &&
        key_size > 1 && key_size <= MAX_API_KEY_BYTES + 1) {
        std::string key(key_size, '\0');
        if (nvs_get_str(handle, NVS_API_KEY, key.data(), &key_size) == ESP_OK) {
            key.resize(key_size - 1);
            config->api_key = std::move(key);
        }
    }

    uint8_t model = static_cast<uint8_t>(DeepSeekModel::Flash);
    if (nvs_get_u8(handle, NVS_MODEL, &model) == ESP_OK &&
        model == static_cast<uint8_t>(DeepSeekModel::Pro)) {
        config->default_model = DeepSeekModel::Pro;
    }
    nvs_close(handle);
    return !config->api_key.empty();
}

bool deepseek_config_save_api_key(const std::string &api_key)
{
    if (api_key.size() > MAX_API_KEY_BYTES) return false;
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = api_key.empty() ? nvs_erase_key(handle, NVS_API_KEY)
                                    : nvs_set_str(handle, NVS_API_KEY, api_key.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND && api_key.empty()) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool deepseek_config_save_default_model(DeepSeekModel model)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(handle, NVS_MODEL, static_cast<uint8_t>(model));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool deepseek_config_has_api_key()
{
    DeepSeekConfig config;
    return deepseek_config_load(&config) && !config.api_key.empty();
}

const char *deepseek_model_id(DeepSeekModel model)
{
    return model == DeepSeekModel::Pro ? "deepseek-v4-pro" : "deepseek-v4-flash";
}

const char *deepseek_model_label(DeepSeekModel model)
{
    return model == DeepSeekModel::Pro ? "V4 Pro" : "V4 Flash";
}

DeepSeekModel deepseek_model_toggle(DeepSeekModel model)
{
    return model == DeepSeekModel::Pro ? DeepSeekModel::Flash : DeepSeekModel::Pro;
}
