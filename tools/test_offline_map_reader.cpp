#include "map/OfflineMap.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
    assert(argc == 3);
    OfflineMap map;
    if (!std::strcmp(argv[2], "invalid")) {
        assert(!map.open(argv[1]));
        std::puts("Invalid pack rejected"); return 0;
    }
    assert(map.open(argv[1]));
    if (!std::strcmp(argv[2], "corrupt")) {
        assert(!map.render(120.10, 30.34, 16, 684, 1216));
        assert(map.error() == OfflineMap::Error::Invalid);
        std::puts("Corrupt tile rejected"); return 0;
    }
    assert(map.supports(16));
    assert(map.render(120.10, 30.34, 16, 684, 1216));
    constexpr size_t size = 684*1216/2;
    std::vector<unsigned char> saved(map.frame(), map.frame()+size);
    assert(map.stats().read > 0);
    assert(map.render(120.10, 30.34, 16, 684, 1216));
    assert(map.stats().read == 0 && map.stats().hits > 0);
    assert(std::memcmp(saved.data(), map.frame(), size) == 0);
    assert(!map.render(0, 0, 16, 684, 1216));
    assert(map.error() == OfflineMap::Error::Range);
    assert(std::memcmp(saved.data(), map.frame(), size) == 0);
    assert(!map.render(120.10, 30.34, 13, 684, 1216));
    assert(std::memcmp(saved.data(), map.frame(), size) == 0);
    FILE *out = std::fopen(argv[2], "wb");
    assert(out && std::fwrite(saved.data(), 1, size, out) == size);
    std::fclose(out);
    std::puts("Offline reader: cache and failed-render frame retention passed");
}
