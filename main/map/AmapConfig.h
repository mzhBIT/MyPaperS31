#pragma once

#include <cstdint>
#include <string>

struct AmapConfig {
    std::string api_key;
    double longitude = 120.10148844719319;
    double latitude = 30.344269032816978;
    uint8_t zoom = 12;
};

bool amap_config_load(AmapConfig *config);
bool amap_config_save_api_key(const std::string &api_key);
bool amap_config_save_viewport(double longitude, double latitude, uint8_t zoom);
