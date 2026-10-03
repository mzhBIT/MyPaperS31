#pragma once
#include "map/AmapClient.h"
#include <functional>
void launch_map_search(const AmapConfig &config, std::function<void(const AmapPlace &)> selected);
void launch_map_lookup(const AmapConfig &config, bool ip, std::function<void(const AmapPlace &)> selected);
