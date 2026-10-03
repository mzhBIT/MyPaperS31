#include "map/MapCoordinates.h"
#include <cassert>
#include <cmath>
#include <cstdio>
int main()
{
    const auto hz = map_coordinates::gcj02_to_wgs84(120.10481, 30.34192);
    assert(std::abs(hz.first-120.1) < 0.0001);
    assert(std::abs(hz.second-30.3442) < 0.0002);
    const auto gcj = map_coordinates::wgs84_to_gcj02(hz.first,hz.second);
    assert(std::abs(gcj.first-120.10481)<1e-7 && std::abs(gcj.second-30.34192)<1e-7);
    const auto outside = map_coordinates::gcj02_to_wgs84(-74.006, 40.7128);
    assert(outside.first == -74.006 && outside.second == 40.7128);
    const auto unchanged = map_coordinates::gcj02_to_wgs84(0, 0);
    assert(unchanged.first == 0 && unchanged.second == 0);
    std::printf("Map coordinates passed: Hangzhou=%0.8f,%0.8f, outside-China unchanged\n", hz.first, hz.second);
}
