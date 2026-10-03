#include "map/OfflineMapSource.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc,char **argv) {
    assert(argc==2||argc==3);
    if (argc==3) {
        OfflineMapSource map;
        assert(!map.is_open());
        assert(map.open(argv[1]));
        assert(map.is_open() && map.count()==0); // Valid catalog, lazy pack load.
        assert(map.render(120.1,30.34,13,684,1216));
        assert(std::strstr(map.active_path(),"overview.mpm"));
        assert(map.render(120.1,30.34,16,684,1216));
        assert(std::strstr(map.active_path(),"detail-"));
        assert(map.render(120.234375-0.000001,30.34,16,684,1216));
        std::string left=map.active_path();
        std::vector<uint8_t> saved(map.frame(),map.frame()+684*1216/2);
        assert(map.render(120.234375,30.34,16,684,1216));
        assert(left!=map.active_path());
        // Crossing the exact integer-pixel boundary shifts the viewport one pixel.
        auto gray=[](const uint8_t *p,size_t i) { return (p[i/2]>>((i&1)?0:4))&15; };
        for (int y=0;y<1216;++y) for (int x=0;x<683;++x)
            assert(gray(saved.data(),y*684+x+1)==gray(map.frame(),y*684+x));
        assert(map.render(120.1,30.34,13,684,1216));
        assert(std::strstr(map.active_path(),"overview.mpm"));
        std::puts("Province: overview/detail switching and real core-boundary pixels passed");
        return 0;
    }
    std::string directory=argv[1];
    OfflineMapSource map;
    assert(!map.is_open());
    assert(map.open((directory+"/catalog.mpc").c_str()));
    assert(map.is_open() && map.count()==0);
    assert(map.supports(3)&&!map.supports(2)&&map.default_zoom()==3);
    assert(map.render(-60,0,3,684,1216));
    assert(std::strstr(map.active_path(),"left.mpm"));
    assert(map.render(60,0,3,684,1216));
    assert(std::strstr(map.active_path(),"right.mpm"));
    assert(map.render(60,0,3,684,1216)&&map.stats().hits>0&&map.stats().read==0);
    assert(map.render(-60,0,3,684,1216));
    std::vector<uint8_t> saved(map.frame(),map.frame()+684*1216/2);
    // Only the isolated fixture file is renamed, never the user's map packages.
    const auto right=directory+"/right.mpm", moved=right+".missing";
    assert(std::rename(right.c_str(),moved.c_str())==0);
    assert(!map.render(110,0,3,684,1216));
    assert(map.is_open()); // A missing region does not close the catalog.
    assert(std::memcmp(saved.data(),map.frame(),saved.size())==0);
    assert(std::rename(moved.c_str(),right.c_str())==0);
    assert(map.render(110,0,3,684,1216));
    assert(!map.render(0,0,2,684,1216));
    map.close();
    assert(!map.is_open());
    assert(map.open((directory+"/left.mpm").c_str()));
    assert(map.is_open() && map.count()>0); // Single packs remain compatible.
    assert(!map.open((directory+"/nonexistent.mpc").c_str()));
    assert(!map.is_open());
    std::puts("Catalog: automatic crossing, uniform tiles, cache, missing-pack frame retention passed");
}
