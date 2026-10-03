#include "map/MapRouteCache.h"
#include <cassert>
#include <cstdio>

int main() {
    MapRouteCache cache;
    cache.start={"起点","地址",120,30}; cache.end={"终点","",121,31};
    AmapRoute output;
    assert(!cache.load(AmapTravelMode::Driving,output));
    for (int i=0;i<3;++i) {
        AmapRoute route;
        route.success=true; route.mode=AmapTravelMode(i);
        route.distance=1000+i; route.duration=60+i;
        route.west=120; route.east=121; route.south=30; route.north=31;
        route.points={{120,30},{121,31}};
        route.steps={"向前行驶",std::string("a\0b",3),""};
        assert(cache.store(route));
    }
    for (int i=0;i<3;++i) {
        assert(cache.load(AmapTravelMode(i),output));
        assert(output.success && output.mode==AmapTravelMode(i));
        assert(output.distance==1000+i && output.duration==60+i);
        assert(output.origin.name=="起点" && output.destination.longitude==121);
        assert(output.points.size()==2 && output.points.back().second==31);
        assert(output.steps.size()==3 && output.steps[1]==std::string("a\0b",3));
        assert(output.west==120 && output.north==31);
    }
    output.distance=5000;
    assert(cache.store(output));
    assert(cache.load(AmapTravelMode::Cycling,output) && output.distance==5000);
    output.success=false;
    assert(!cache.store(output));
    assert(cache.load(AmapTravelMode::Cycling,output) && output.distance==5000);
    output.steps={std::string(2049,'x')};
    assert(!cache.store(output));
    assert(cache.load(AmapTravelMode::Cycling,output) && output.steps.size()==3);
    cache.clear();
    for (int i=0;i<3;++i) assert(!cache.load(AmapTravelMode(i),output));
    std::puts("Map route cache passed: modes, geometry, UTF-8, refresh, failure retention, invalidation");
}
