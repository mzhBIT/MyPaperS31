#pragma once

#include <cstdint>
#include <string>

struct AmapConfig {
    std::string api_key;
    double longitude = 120.10148844719319;
    double latitude = 30.344269032816978;
    uint8_t zoom = 12;
    bool offline = false;
    std::string offline_path;
    bool offline_viewport_saved = false;
};

bool amap_config_load(AmapConfig *config);
bool amap_config_save_api_key(const std::string &api_key);
bool amap_config_save_viewport(double longitude, double latitude, uint8_t zoom, bool offline = false);
bool amap_config_save_source(bool offline);
bool amap_config_save_offline_path(const std::string &path);
