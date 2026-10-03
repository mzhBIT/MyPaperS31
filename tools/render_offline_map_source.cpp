// Portable integration probe: catalog/pack lon lat zoom output.gray4
#include "map/OfflineMapSource.h"
#include <cstdio>
#include <cstdlib>
int main(int argc,char **argv) {
    if (argc!=6) return 2;
    OfflineMapSource map;
    if (!map.open(argv[1])||!map.render(std::atof(argv[2]),std::atof(argv[3]),
                                     static_cast<uint8_t>(std::atoi(argv[4])),684,1216)) {
        std::fprintf(stderr,"Reader error %d\n",int(map.error())); return 1;
    }
    FILE *f=std::fopen(argv[5],"wb");
    if (!f) return 1;
    const bool ok=std::fwrite(map.frame(),1,415872,f)==415872;
    std::fclose(f);
    std::printf("Active pack: %s; reads=%u hits=%u\n",map.active_path(),map.stats().read,map.stats().hits);
    return ok?0:1;
}
