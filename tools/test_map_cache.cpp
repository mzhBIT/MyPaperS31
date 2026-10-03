#include "map/MapCache.h"
#include "map/MapCrc.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

int main(int argc,char **argv) {
    const char *known="123456789";
    assert(~map_crc::update(~0u,known,9)==0xcbf43926u);
    assert(~map_crc::update(map_crc::update(~0u,known,4),known+4,5)==0xcbf43926u);
    assert(argc==2);
    // A new, explicitly supplied test directory only; never touch SD packages.
    const std::filesystem::path root=argv[1];
    assert(!std::filesystem::exists(root));
    std::filesystem::create_directories(root);
    const auto source=(root/"source.png").string();
    { std::ofstream f(source,std::ios::binary); f<<std::string(64,'x'); }
    const auto untouched=(root/"overview.mpm").string();
    { std::ofstream f(untouched); f<<"user map"; }
    AmapConfig a; a.longitude=120.1; a.latitude=30.34; a.zoom=13;
    const auto k1=MapCache::key(a,684,1216,1);
    assert(k1.rfind("amap3_",0)==0 && k1.find("_z13_r11_")!=std::string::npos);
    a.zoom=19; assert(!MapCache::key(a,684,1216,1).empty());
    a.zoom=20; assert(MapCache::key(a,684,1216,1).empty());
    a.zoom=1; assert(MapCache::key(a,684,1216,1).empty());
    a.zoom=13;
    a.api_key="secret-not-for-cache";
    assert(k1==MapCache::key(a,684,1216,1));
    assert(k1.find(a.api_key)==std::string::npos);
    assert(k1!=MapCache::key(a,684,1216,2));
    assert(k1!=MapCache::key(a,680,1216,1));
    a.zoom=14; const auto k2=MapCache::key(a,684,1216,1);
    a.zoom=15; const auto k3=MapCache::key(a,684,1216,1);
    a.offline=true; assert(MapCache::key(a,684,1216,1).empty());
    MapCache cache(root.string(),192,2); // Exactly two 64-byte payloads + metadata.
    std::string path;
    assert(!cache.lookup(k1,&path));
    assert(cache.store(k1,source.c_str(),&path));
    assert(cache.store(k2,source.c_str(),&path));
    assert(cache.usage().entries==2 && cache.usage().bytes==192);
    assert(cache.lookup(k1,&path)); // k2 is now least recently used.
    assert(cache.store(k3,source.c_str(),&path));
    assert(!cache.lookup(k2,&path));
    MapCache reopened(root.string(),192,2);
    assert(reopened.lookup(k1,&path)); // Identity and LRU survive reopening.
    { std::ofstream f(path,std::ios::binary); f<<std::string(64,'z'); }
    assert(!reopened.lookup(k1,&path)); // Same size, CRC mismatch.
    // A dropped commit marker or unfinished write is never a hit, and is cleaned.
    { std::ofstream f(root/(k1+".img.tmp")); f<<"partial"; }
    { std::ofstream f(root/(k1+".img")); f<<"orphan"; }
    reopened.reset_index(); // Simulate restarting after a dropped commit.
    assert(reopened.store(k2,source.c_str(),&path));
    assert(!std::filesystem::exists(root/(k1+".img.tmp")));
    assert(!std::filesystem::exists(root/(k1+".img")));
    assert(!reopened.invalidate("../overview"));
    assert(reopened.clear());
    assert(reopened.usage().entries==0);
    assert(std::filesystem::exists(untouched) && std::filesystem::exists(source));
    MapCache too_small(root.string(),95,2);
    assert(!too_small.store(k1,source.c_str(),&path));
    assert(!std::filesystem::exists(root/(k1+".img.tmp")));
    MapCache one_entry(root.string(),4096,1);
    assert(one_entry.store(k1,source.c_str(),&path));
    assert(one_entry.store(k2,source.c_str(),&path));
    assert(one_entry.usage().entries==1 && !one_entry.lookup(k1,&path));
    { std::ofstream f(root/(k2+".meta"),std::ios::binary); f<<"truncated"; }
    assert(!one_entry.lookup(k2,&path));
    assert(!one_entry.store("../escape",source.c_str(),&path));
    const auto download=(root/"download.tmp").string();
    const std::string payload(64,'a');
    const uint32_t crc=~map_crc::update(~0u,payload.data(),payload.size());
    { std::ofstream f(download,std::ios::binary); f<<payload; }
    assert(!one_entry.store_download(k1,download.c_str(),65,crc,&path));
    assert(std::filesystem::exists(download)); // Failed size check retains fallback.
    assert(!too_small.store_download(k1,download.c_str(),64,crc,&path));
    assert(std::filesystem::exists(download)); // Capacity rejection also retains source.
    assert(one_entry.store_download(k1,download.c_str(),64,crc,&path));
    assert(!std::filesystem::exists(download)); // Ownership was transferred, not copied.
    assert(one_entry.lookup(k1,&path));
    { std::ofstream f(download,std::ios::binary); f<<std::string(64,'b'); }
    assert(one_entry.store_download(k2,download.c_str(),64,
        ~map_crc::update(~0u,std::string(64,'b').data(),64),&path));
    assert(one_entry.usage().entries==1 && !one_entry.lookup(k1,&path));
    assert(one_entry.lookup(k2,&path));
    { std::ofstream f(path,std::ios::binary); f<<std::string(64,'c'); }
    assert(!one_entry.lookup(k2,&path)); // Adopted files still get CRC validation.
    // Metadata publication failure must return the downloaded file to the caller.
    { std::ofstream f(download,std::ios::binary); f<<payload; }
    const auto blocked_meta=root/(k1+".meta.tmp");
    std::filesystem::create_directory(blocked_meta);
    assert(!one_entry.store_download(k1,download.c_str(),64,crc,&path));
    assert(std::filesystem::exists(download));
    assert(!std::filesystem::exists(root/(k1+".img")));
    std::filesystem::remove(blocked_meta); // This test's empty failure-injection directory only.
    assert(one_entry.store_download(k1,download.c_str(),64,crc,&path));
    assert(one_entry.lookup(k1,&path));
    assert(one_entry.clear());
    std::puts("Map cache: CRC streaming, zero-copy adoption, failure fallback, LRU, byte cap, recovery and safe clear passed");
}
