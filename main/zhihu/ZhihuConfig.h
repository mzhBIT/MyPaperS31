#pragma once

#include <string>

struct ZhihuConfig {
    std::string access_secret;
};

bool zhihu_config_load(ZhihuConfig *config);
bool zhihu_config_save_access_secret(const std::string &access_secret);
bool zhihu_config_has_access_secret();
