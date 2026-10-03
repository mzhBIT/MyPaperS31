#include "map/AmapRoute.h"
#include <cassert>
#include <cstdio>

int main()
{
    auto route=parse_amap_route(R"({"status":"1","route":{"paths":[{"distance":"1234","cost":{"duration":"600"},"steps":[{"instruction":"Go straight","polyline":"120,30;120.1,30.1"},{"instruction":"Turn left","polyline":"120.1,30.1;120.2,30.2"}]}]}})");
    assert(route.success && route.distance==1234 && route.duration==600 && route.steps.size()==2 && route.points.size()==3);
    assert(route.west==120 && route.east==120.2);
    assert(!parse_amap_route(R"({"status":"1","route":{"paths":[]}})").success);
    assert(!parse_amap_route(R"({"status":"1","route":{"paths":[{"distance":"-1","steps":[]}]}})").success);
    assert(!parse_amap_route(R"({"status":"1","route":{"paths":[{"distance":"1","steps":[{"polyline":"nan,30"}]}]}})").success);
    assert(!parse_amap_route(R"({"status":"0","infocode":"10001"})").success);
    std::string line;
    for (int i=0;i<3000;++i) { if (i) line+=';'; line+=std::to_string(120.0+i/100000.0)+",30"; }
    route=parse_amap_route("{\"status\":\"1\",\"route\":{\"paths\":[{\"distance\":\"2000\",\"steps\":[{\"instruction\":\"walk\",\"polyline\":\""+line+"\"}]}]}}");
    assert(route.success && route.points.size()<=512 && route.points.front().first==120);
    assert(route.points.back().first>120.02998 && route.duration==-1);
    std::puts("AMap route passed: summary, steps, geometry, empty/errors and bounded simplification");
}
