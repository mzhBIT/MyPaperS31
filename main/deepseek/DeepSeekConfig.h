#pragma once

#include <cstddef>
#include <string>

enum class DeepSeekModel {
    Flash = 0,
    Pro = 1,
};

struct DeepSeekConfig {
    std::string api_key;
    DeepSeekModel default_model = DeepSeekModel::Flash;
};

bool deepseek_config_load(DeepSeekConfig *config);
bool deepseek_config_save_api_key(const std::string &api_key);
bool deepseek_config_save_default_model(DeepSeekModel model);
bool deepseek_config_has_api_key();

const char *deepseek_model_id(DeepSeekModel model);
const char *deepseek_model_label(DeepSeekModel model);
DeepSeekModel deepseek_model_toggle(DeepSeekModel model);
