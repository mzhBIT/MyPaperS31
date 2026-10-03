#include "map/MapSourcePolicy.h"
#include "map/MapScale.h"
#include <cassert>
#include <cmath>
#include <cstdio>
int main()
{
    for (uint8_t z=3;z<=19;++z) {
        assert(map_scale::request_zoom(z)==z-2);
        assert(map_scale::world_pixels(z)==4*map_scale::world_pixels(map_scale::request_zoom(z)));
        assert(map_scale::world_pixels(z)/map_scale::world_pixels(z-1)==2);
    }
    AmapConfig c; c.hybrid=true; c.offline=true;
    const double lon=c.longitude,lat=c.latitude;
    assert(!map_source_policy::apply_zoom(c,16) && c.offline);
    assert(map_source_policy::apply_zoom(c,17) && !c.offline);
    assert(!map_source_policy::apply_zoom(c,18) && !c.offline);
    const auto gcj=map_coordinates::wgs84_to_gcj02(lon,lat);
    assert(c.longitude==gcj.first && c.latitude==gcj.second);
    assert(!map_source_policy::apply_zoom(c,17));
    assert(map_source_policy::apply_zoom(c,16) && c.offline);
    for (int i=0;i<100;++i) {
        map_source_policy::apply_zoom(c,17);
        map_source_policy::apply_zoom(c,16);
    }
    assert(std::abs(c.longitude-lon)<0.000001 && std::abs(c.latitude-lat)<0.000001);
    c.hybrid=false; c.offline=true;
    assert(!map_source_policy::apply_zoom(c,17) && c.offline);
    c.offline=false;
    assert(!map_source_policy::apply_zoom(c,16) && !c.offline);
    c.hybrid=true; c.offline=true; c.longitude=-74.006; c.latitude=40.7128;
    map_source_policy::apply_zoom(c,17);
    assert(c.longitude==-74.006 && c.latitude==40.7128);
    std::puts("Map source policy passed: scale 2x/level, request/display zoom, Z16/Z17/Z18, pure modes and coordinates");
}
