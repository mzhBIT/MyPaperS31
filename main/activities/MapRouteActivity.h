#pragma once
#include "map/AmapRoute.h"
#include <functional>
#include <memory>
#include "map/MapRouteCache.h"
void launch_map_route(const AmapConfig &config, std::function<void(AmapRoute)> show,
                      std::shared_ptr<MapRouteCache> cache);
