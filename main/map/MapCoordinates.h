#pragma once
#include <utility>

namespace map_coordinates {
std::pair<double, double> wgs84_to_gcj02(double longitude, double latitude);
std::pair<double, double> gcj02_to_wgs84(double longitude, double latitude);
}
