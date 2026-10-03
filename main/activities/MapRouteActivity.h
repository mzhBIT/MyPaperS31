#pragma once
#include "map/AmapRoute.h"
#include <functional>
void launch_map_route(const AmapConfig &config, std::function<void(AmapRoute)> show);
