#include "map/MapCoordinates.h"
#include <cmath>

namespace map_coordinates {
namespace {
constexpr double PI = 3.14159265358979323846;
std::pair<double,double> forward(double lon, double lat)
{
    const double x = lon-105, y = lat-35;
    double dy = -100+2*x+3*y+0.2*y*y+0.1*x*y+0.2*std::sqrt(std::abs(x));
    dy += (20*std::sin(6*x*PI)+20*std::sin(2*x*PI))*2/3;
    dy += (20*std::sin(y*PI)+40*std::sin(y*PI/3))*2/3;
    dy += (160*std::sin(y*PI/12)+320*std::sin(y*PI/30))*2/3;
    double dx = 300+x+2*y+0.1*x*x+0.1*x*y+0.1*std::sqrt(std::abs(x));
    dx += (20*std::sin(6*x*PI)+20*std::sin(2*x*PI))*2/3;
    dx += (20*std::sin(x*PI)+40*std::sin(x*PI/3))*2/3;
    dx += (150*std::sin(x*PI/12)+300*std::sin(x*PI/30))*2/3;
    const double rad = lat*PI/180, sinlat = std::sin(rad);
    const double magic = 1-0.00669342162296594323*sinlat*sinlat;
    const double root = std::sqrt(magic);
    dy = dy*180/((6378245.0*(1-0.00669342162296594323))/(magic*root)*PI);
    dx = dx*180/(6378245.0/root*std::cos(rad)*PI);
    return {lon+dx, lat+dy};
}
}
std::pair<double,double> wgs84_to_gcj02(double lon, double lat)
{
    if (!std::isfinite(lon) || !std::isfinite(lat) || lon < 72.004 || lon > 137.8347 ||
        lat < 0.8293 || lat > 55.8271) return {lon, lat};
    return forward(lon, lat);
}
std::pair<double,double> gcj02_to_wgs84(double lon, double lat)
{
    if (!std::isfinite(lon) || !std::isfinite(lat) || lon < 72.004 || lon > 137.8347 ||
        lat < 0.8293 || lat > 55.8271) return {lon, lat};
    double x = lon, y = lat;
    // Fixed-point inverse; approximation for offline OSM display, not surveying.
    for (int i = 0; i < 8; ++i) {
        const auto projected = forward(x, y);
        const double dx = projected.first-lon, dy = projected.second-lat;
        x -= dx; y -= dy;
        if (std::abs(dx)+std::abs(dy) < 1e-8) break;
    }
    return {x, y};
}
}
