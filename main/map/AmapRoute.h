#pragma once
#include "map/AmapClient.h"
#include <utility>

enum class AmapTravelMode { Driving, Walking, Cycling };
struct AmapRoute {
    bool success = false;
    std::string error;
    AmapPlace origin, destination;
    AmapTravelMode mode = AmapTravelMode::Driving;
    double distance = 0;
    int duration = -1;
    std::vector<std::string> steps;
    std::vector<std::pair<double,double>> points; // GCJ-02, max 512.
    double west=180, south=90, east=-180, north=-90;
};
AmapRoute parse_amap_route(std::string_view body);
