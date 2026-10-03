#include "map/OfflineMapSource.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_log.h>
#define SOURCE_LOG(...) ESP_LOGW("OFFLINE_SOURCE", __VA_ARGS__)
#define SOURCE_INFO(...) ESP_LOGI("OFFLINE_SOURCE", __VA_ARGS__)
#else
#define SOURCE_LOG(...) do { std::fprintf(stderr, "OFFLINE_SOURCE: "); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define SOURCE_INFO(...) SOURCE_LOG(__VA_ARGS__)
#endif

namespace {
constexpr size_t RECORD = 136;
uint16_t u16(const uint8_t *p) { return p[0] | uint16_t(p[1])<<8; }
uint32_t u32(const uint8_t *p) { return p[0] | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
double f64(const uint8_t *p) {
    uint64_t b = u32(p) | uint64_t(u32(p+4))<<32;
    double v; std::memcpy(&v,&b,8); return v;
}
uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    while (n--) { c ^= *p++; for (int i=0;i<8;++i) c = (c>>1)^((c&1)?0xedb88320u:0); }
    return ~c;
}
bool valid_bounds(const uint8_t *p) {
    const double w=f64(p),s=f64(p+8),e=f64(p+16),n=f64(p+24);
    return std::isfinite(w)&&std::isfinite(s)&&std::isfinite(e)&&std::isfinite(n)&&
        -180<=w&&w<e&&e<=180&&-85.051129<=s&&s<n&&n<=85.051129;
}
bool contains(const uint8_t *p,double lon,double lat) {
    return lon>=f64(p)&&lon<f64(p+16)&&lat>=f64(p+8)&&lat<f64(p+24);
}
}

void OfflineMapSource::close() {
    _opened=false;
    _reader.close(); std::free(_records); _records=nullptr; _count=_zooms=0;
    _active=-1; _directory.clear(); _active_path.clear();
}

bool OfflineMapSource::open(const char *path) {
    close(); _error=OfflineMap::Error::None;
    if (!path) return fail(OfflineMap::Error::Invalid);
    FILE *f=std::fopen(path,"rb");
    if (!f) { SOURCE_LOG("open failed path=%s errno=%d", path, errno); return fail(OfflineMap::Error::Io); }
    uint8_t h[32];
    const bool catalog=std::fread(h,1,32,f)==32&&!std::memcmp(h,"MPCAT001",8);
    if (!catalog) {
        std::fclose(f);
        if (!_reader.open(path)) return fail(_reader.error());
        for (uint8_t z=0;z<=22;++z) if (_reader.supports(z)) _zooms|=1u<<z;
        _min=_reader.min_zoom(); _max=_default=_reader.max_zoom();
        _lon=_reader.center_lon(); _lat=_reader.center_lat(); _active_path=path;
        _opened=true;
        return true;
    }
    const char *stage="catalog-header";
    auto reject=[&](OfflineMap::Error e) {
        SOURCE_LOG("catalog rejected path=%s stage=%s error=%d errno=%d", path, stage, int(e), errno);
        std::fclose(f); close(); return fail(e);
    };
    const uint32_t count=u32(h+16);
    SOURCE_INFO("catalog header path=%s version=%u header=%u record=%lu count=%lu", path,
               unsigned(u16(h+8)), unsigned(u16(h+10)), (unsigned long)u32(h+12), (unsigned long)count);
    if (u16(h+8)!=1||u16(h+10)!=32||u32(h+12)!=RECORD||!count||count>4096||u32(h+24)!=1||u32(h+28))
        return reject(OfflineMap::Error::Invalid);
    const size_t bytes=count*RECORD;
    stage="catalog-size";
    if (std::fseek(f,0,SEEK_END)) return reject(OfflineMap::Error::Io);
    const long actual_size=std::ftell(f);
    if (actual_size<0) return reject(OfflineMap::Error::Io);
    if (actual_size!=long(32+bytes)) {
        SOURCE_LOG("catalog size actual=%ld expected=%lu", actual_size, (unsigned long)(32+bytes));
        return reject(OfflineMap::Error::Invalid);
    }
    if (std::fseek(f,32,SEEK_SET)) return reject(OfflineMap::Error::Io);
    stage="catalog-allocation";
#ifdef ESP_PLATFORM
    _records=static_cast<uint8_t *>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
#else
    _records=static_cast<uint8_t *>(std::malloc(bytes));
#endif
    if (!_records) return reject(OfflineMap::Error::Memory);
    stage="catalog-record-read";
    if (std::fread(_records,1,bytes,f)!=bytes) return reject(OfflineMap::Error::Io);
    stage="catalog-crc";
    const uint32_t actual_crc=crc(_records,bytes);
    if (actual_crc!=u32(h+20)) {
        SOURCE_LOG("catalog CRC actual=%08lx expected=%08lx", (unsigned long)actual_crc, (unsigned long)u32(h+20));
        return reject(OfflineMap::Error::Invalid);
    }
    stage="catalog-record-validation";
    for (uint32_t i=0;i<count;++i) {
        const uint8_t *r=_records+i*RECORD;
        const char *name=reinterpret_cast<const char *>(r);
        const void *end=std::memchr(name,0,64);
        uint32_t mask=u32(r+64);
        if (!end||!name[0]||std::strchr(name,'/')||std::strchr(name,'\\')||std::strstr(name,"..")||
            !mask||(mask&~0x7fffffu)||u32(r+68)||!valid_bounds(r+72)||!valid_bounds(r+104))
        {
            SOURCE_LOG("invalid catalog record index=%lu name=%.64s mask=%08lx", (unsigned long)i, name, (unsigned long)mask);
            return reject(OfflineMap::Error::Invalid);
        }
        const size_t length=std::strlen(name);
        if (length<4||std::strcmp(name+length-4,".mpm")) {
            SOURCE_LOG("invalid pack suffix index=%lu name=%s", (unsigned long)i, name);
            return reject(OfflineMap::Error::Invalid);
        }
        _zooms|=mask;
    }
    std::fclose(f); _count=count;
    std::string source=path;
    const size_t slash=source.find_last_of("/\\");
    _directory=slash==std::string::npos?"":source.substr(0,slash+1);
    _min=0; while (!supports(_min)) ++_min;
    _max=22; while (!supports(_max)) --_max;
    // First catalog entry is the overview; start there, not at a random detail block.
    _default=22; while (!(u32(_records+64)&(1u<<_default))) --_default;
    _lon=(f64(_records+72)+f64(_records+88))/2;
    _lat=(f64(_records+80)+f64(_records+96))/2;
    _opened=true;
    return true;
}

bool OfflineMapSource::render(double lon,double lat,uint8_t z,int w,int h) {
    _error=OfflineMap::Error::None;
    if (!_records) {
        if (!_reader.render(lon,lat,z,w,h)) return fail(_reader.error());
        return true;
    }
    if (!std::isfinite(lon)||!std::isfinite(lat)||!supports(z)) return fail(OfflineMap::Error::Range);
    OfflineMap::Error last=OfflineMap::Error::Range;
    // Core owner first, then overlapping neighbours. Never publish a partial frame.
    for (int pass=0;pass<2;++pass) for (uint32_t i=0;i<_count;++i) {
        const uint8_t *r=_records+i*RECORD;
        const bool core=contains(r+72,lon,lat);
        if (!(u32(r+64)&(1u<<z))||!contains(r+104,lon,lat)||core!=(pass==0)) continue;
        if (_active==int(i)) {
            if (_reader.render(lon,lat,z,w,h)) return true;
            last=_reader.error();
        } else {
            const std::string path=_directory+reinterpret_cast<const char *>(r);
            OfflineMap candidate;
            if (candidate.open(path.c_str())&&candidate.render(lon,lat,z,w,h)) {
                _reader.swap(candidate); _active=int(i); _active_path=path; return true;
            }
            last=candidate.error();
            SOURCE_LOG("member failed path=%s error=%d zoom=%u center=%.6f,%.6f",
                       path.c_str(), int(last), unsigned(z), lon, lat);
        }
        if (last==OfflineMap::Error::Memory||last==OfflineMap::Error::Invalid) return fail(last);
    }
    return fail(last);
}
