#include "map/OfflineMap.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <cerrno>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_log.h>
#define PACK_LOG(...) ESP_LOGW("OFFLINE_PACK", __VA_ARGS__)
#define PACK_INFO(...) ESP_LOGI("OFFLINE_PACK", __VA_ARGS__)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#define PACK_LOG(...) do { std::fprintf(stderr, "OFFLINE_PACK: "); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define PACK_INFO(...) PACK_LOG(__VA_ARGS__)
#endif

namespace {
constexpr uint32_t TILE_BYTES = 32768;
constexpr double PI = 3.14159265358979323846;
uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t u64(const uint8_t *p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }
double f64(const uint8_t *p) { uint64_t bits = u64(p); double value; std::memcpy(&value, &bits, 8); return value; }
uint8_t *allocate(size_t bytes) {
#ifdef ESP_PLATFORM
    return static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    return static_cast<uint8_t *>(std::malloc(bytes));
#endif
}
void yield_work() {
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#endif
}
uint32_t crc32(const uint8_t *p, size_t size) {
    static const auto table = [] {
        struct Table { uint32_t v[256]; } t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
            t.v[i] = c;
        }
        return t;
    }();
    uint32_t c = ~0u;
    for (size_t i = 0; i < size; ++i) {
        c = table.v[(c ^ p[i]) & 255] ^ (c >> 8);
        if (i && i % 262144 == 0) yield_work();
    }
    return ~c;
}
bool read_at(FILE *f, uint64_t offset, uint8_t *data, size_t bytes) {
    // IDF's current FAT stdio uses long offsets. Explicitly reject >2GiB on
    // 32-bit builds rather than silently wrap; v1 test packs are much smaller.
    if (offset > uint64_t(LONG_MAX) || std::fseek(f, static_cast<long>(offset), SEEK_SET)) return false;
    return std::fread(data, 1, bytes, f) == bytes;
}
}

void OfflineMap::close() {
    if (_file) std::fclose(_file);
    _file = nullptr;
    std::free(_index); _index = nullptr;
    std::free(_frame); _frame = nullptr;
    std::free(_scratch); _scratch = nullptr;
    for (auto &c : _cache) { std::free(c.data); c = CachedTile{}; }
    _count = _zooms = 0; _width = _height = 0; _clock = 0;
}

void OfflineMap::swap(OfflineMap &o) {
    using std::swap;
    swap(_file, o._file); swap(_index, o._index); swap(_frame, o._frame); swap(_scratch, o._scratch);
    swap(_count, o._count); swap(_zooms, o._zooms); swap(_min, o._min); swap(_max, o._max);
    for (int i = 0; i < 4; ++i) swap(_bounds[i], o._bounds[i]);
    swap(_width, o._width); swap(_height, o._height); swap(_clock, o._clock);
    for (int i = 0; i < CACHE_SLOTS; ++i) swap(_cache[i], o._cache[i]);
    swap(_error, o._error); swap(_stats, o._stats);
}

bool OfflineMap::open(const char *path) {
    close(); _error = Error::None;
    if (!path) return fail(Error::Invalid);
    _file = std::fopen(path, "rb");
    if (!_file) { PACK_LOG("open failed path=%s errno=%d", path, errno); return fail(Error::Io); }
    const char *stage="header-read";
    auto invalid = [this, path, &stage](Error e) {
        PACK_LOG("pack rejected path=%s stage=%s error=%d errno=%d", path, stage, int(e), errno);
        close(); return fail(e);
    };
    uint8_t h[96];
    if (!read_at(_file, 0, h, sizeof(h))) return invalid(Error::Io);
    const uint16_t version = u16(h+8);
    stage="header-format";
    PACK_INFO("pack header path=%s magic=%.8s version=%u count=%lu", path,
             reinterpret_cast<const char *>(h), unsigned(version), (unsigned long)u32(h+16));
    if (std::memcmp(h, "MPMAP001", 8) || (version != 1 && version != 2) || u16(h+10) != 96 ||
        u32(h+12) != 256 || u32(h+28) != 1 || u64(h+64) != 96 || u32(h+80) != 32 ||
        u64(h+88) != 0) return invalid(Error::Invalid);
    _count = u32(h+16);
    stage="zoom-count";
    const uint32_t minz = u32(h+20), maxz = u32(h+24);
    if (!_count || _count > 131072 || minz > maxz || maxz > 22) return invalid(Error::Invalid);
    _min = static_cast<uint8_t>(minz); _max = static_cast<uint8_t>(maxz);
    stage="bounds";
    for (int i = 0; i < 4; ++i) { _bounds[i] = f64(h+32+i*8); if (!std::isfinite(_bounds[i])) return invalid(Error::Invalid); }
    if (!(-180 <= _bounds[0] && _bounds[0] < _bounds[2] && _bounds[2] <= 180 &&
          -85.051129 <= _bounds[1] && _bounds[1] < _bounds[3] && _bounds[3] <= 85.051129)) return invalid(Error::Invalid);
    uint64_t data_offset = 96ull + _count * 32ull;
    stage="data-offset-size";
    if (u64(h+72) != data_offset || std::fseek(_file, 0, SEEK_END)) return invalid(Error::Invalid);
    const long file_size = std::ftell(_file);
    PACK_INFO("pack size actual=%ld index_bytes=%lu data_offset=%llu", file_size,
             (unsigned long)(_count*32), (unsigned long long)data_offset);
    if (file_size < 0 || uint64_t(file_size) < data_offset) return invalid(Error::Invalid);
    stage="index-allocation";
    _index = allocate(size_t(_count) * 32);
    if (!_index) return invalid(Error::Memory);
    stage="index-read";
    if (!read_at(_file, 96, _index, size_t(_count)*32)) return invalid(Error::Io);
    stage="index-crc";
    const uint32_t actual_crc=crc32(_index, size_t(_count)*32);
    if (actual_crc != u32(h+84)) {
        PACK_LOG("index CRC actual=%08lx expected=%08lx", (unsigned long)actual_crc, (unsigned long)u32(h+84));
        return invalid(Error::Invalid);
    }
    stage="index-entry";
    uint64_t previous = 0;
    uint64_t cursor = data_offset;
    for (uint32_t i = 0; i < _count; ++i) {
        const uint8_t *e = _index + i*32;
        uint32_t x = u32(e+4), y = u32(e+8);
        uint8_t z = e[0];
        uint64_t key = (uint64_t(z)<<44) | (uint64_t(x)<<22) | y;
        if (z < _min || z > _max || x >= (1u<<z) || y >= (1u<<z) ||
            (i && key <= previous) || u32(e+28)) {
            PACK_LOG("invalid tile index=%lu z=%u x=%lu y=%lu", (unsigned long)i, unsigned(z), (unsigned long)x, (unsigned long)y);
            return invalid(Error::Invalid);
        }
        const bool uniform = version == 2 && e[1] == 1 && u16(e+2) <= 15 && u64(e+12) == 0 && u32(e+20) == 0;
        const bool raw = e[1] == 0 && u16(e+2) == 0 && u32(e+20) == TILE_BYTES && u64(e+12) == cursor;
        if (!uniform && !raw) {
            PACK_LOG("invalid tile layout index=%lu encoding=%u offset=%llu expected=%llu bytes=%lu",
                     (unsigned long)i, unsigned(e[1]), (unsigned long long)u64(e+12),
                     (unsigned long long)cursor, (unsigned long)u32(e+20));
            return invalid(Error::Invalid);
        }
        if (raw) cursor += TILE_BYTES;
        _zooms |= 1u<<z; previous = key;
        if (i && i % 8192 == 0) yield_work();
    }
    stage="payload-size";
    if (cursor != uint64_t(file_size) || !supports(_min) || !supports(_max)) {
        PACK_LOG("payload size actual=%ld expected=%llu", file_size, (unsigned long long)cursor);
        return invalid(Error::Invalid);
    }
    return true;
}

const uint8_t *OfflineMap::tile(uint8_t zoom, uint32_t x, uint32_t y) {
    uint32_t lo = 0, hi = _count;
    uint64_t key = (uint64_t(zoom)<<44) | (uint64_t(x)<<22) | y;
    while (lo < hi) {
        uint32_t mid = lo + (hi-lo)/2;
        const uint8_t *e = _index + mid*32;
        uint64_t current = (uint64_t(e[0])<<44) | (uint64_t(u32(e+4))<<22) | u32(e+8);
        if (current < key) lo = mid+1; else hi = mid;
    }
    if (lo == _count) { fail(Error::Missing); return nullptr; }
    const uint8_t *e = _index + lo*32;
    if (e[0] != zoom || u32(e+4) != x || u32(e+8) != y) { fail(Error::Missing); return nullptr; }
    for (auto &c : _cache) if (c.entry == lo) { c.age = ++_clock; ++_stats.hits; return c.data; }
    auto &c = *std::min_element(std::begin(_cache), std::end(_cache), [](const CachedTile &a, const CachedTile &b) { return a.age < b.age; });
    if (!c.data) c.data = allocate(TILE_BYTES);
    if (!c.data) { fail(Error::Memory); return nullptr; }
    c.entry = UINT32_MAX;
    if (e[1] == 1) std::memset(c.data, e[2] * 17, TILE_BYTES);
    else if (!read_at(_file, u64(e+12), c.data, TILE_BYTES)) {
        PACK_LOG("tile read failed z=%u x=%lu y=%lu offset=%llu errno=%d", unsigned(zoom),
                 (unsigned long)x, (unsigned long)y, (unsigned long long)u64(e+12), errno);
        fail(Error::Io); return nullptr;
    }
    if (crc32(c.data, TILE_BYTES) != u32(e+24)) {
        PACK_LOG("tile CRC failed z=%u x=%lu y=%lu", unsigned(zoom), (unsigned long)x, (unsigned long)y);
        fail(Error::Invalid); return nullptr;
    }
    c.entry = lo; c.age = ++_clock; if (!e[1]) ++_stats.read;
    return c.data;
}

bool OfflineMap::render(double lon, double lat, uint8_t zoom, int width, int height) {
    _error = Error::None; _stats = {};
    if (!_file || !std::isfinite(lon) || !std::isfinite(lat) || !supports(zoom) ||
        width < 1 || width > 2048 || height < 1 || height > 2048 || width % 2) return fail(Error::Invalid);
    if (lon < _bounds[0] || lon > _bounds[2] || lat < _bounds[1] || lat > _bounds[3]) return fail(Error::Range);
    double world = 256.0 * (1u << zoom);
    int64_t left = static_cast<int64_t>(std::floor((lon+180)/360*world-width/2.0));
    int64_t top = static_cast<int64_t>(std::floor((1-std::asinh(std::tan(lat*PI/180))/PI)/2*world-height/2.0));
    if (left < 0 || top < 0 || left+width > world || top+height > world) return fail(Error::Range);
    if (_width != width || _height != height) {
        uint8_t *a = allocate(size_t(width)*height/2), *b = allocate(size_t(width)*height/2);
        if (!a || !b) { std::free(a); std::free(b); return fail(Error::Memory); }
        std::free(_frame); std::free(_scratch); _frame = a; _scratch = b; _width = width; _height = height;
    }
    std::memset(_scratch, 0xff, size_t(width)*height/2);
    for (int64_t ty = top/256; ty <= (top+height-1)/256; ++ty) {
        for (int64_t tx = left/256; tx <= (left+width-1)/256; ++tx) {
            const uint8_t *data = tile(zoom, static_cast<uint32_t>(tx), static_cast<uint32_t>(ty));
            if (!data) return false; // Existing completed frame is not changed.
            int x0 = static_cast<int>(std::max(left, tx*256)-left);
            int x1 = static_cast<int>(std::min(left+width, (tx+1)*256)-left);
            int y0 = static_cast<int>(std::max(top, ty*256)-top);
            int y1 = static_cast<int>(std::min(top+height, (ty+1)*256)-top);
            for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
                int src = static_cast<int>((top+y-ty*256)*256+(left+x-tx*256));
                uint8_t gray = (data[src/2] >> ((src&1) ? 0 : 4)) & 15;
                size_t dst = size_t(y)*width+x;
                uint8_t &v = _scratch[dst/2];
                v = (dst&1) ? ((v&0xf0)|gray) : ((v&15)|(gray<<4));
            }
        }
    }
    std::swap(_frame, _scratch);
    return true;
}
