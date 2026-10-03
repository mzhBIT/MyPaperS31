#pragma once
#include <cstdint>
#include <cstdio>

// Portable reader for tools/build_offline_map.py. No display or network calls.
class OfflineMap {
public:
    enum class Error { None, Invalid, Io, Memory, Range, Missing };
    struct Stats { uint32_t read = 0, hits = 0; };
    OfflineMap() = default;
    ~OfflineMap() { close(); }
    OfflineMap(const OfflineMap &) = delete;
    OfflineMap &operator=(const OfflineMap &) = delete;
    bool open(const char *path);
    void close();
    void swap(OfflineMap &other);
    bool render(double longitude, double latitude, uint8_t zoom, int width, int height);
    const uint8_t *frame() const { return _frame; }
    Error error() const { return _error; }
    bool supports(uint8_t zoom) const { return zoom <= 22 && (_zooms & (1u << zoom)); }
    double center_lon() const { return (_bounds[0] + _bounds[2]) / 2; }
    double center_lat() const { return (_bounds[1] + _bounds[3]) / 2; }
    uint8_t min_zoom() const { return _min; }
    uint8_t max_zoom() const { return _max; }
    uint32_t count() const { return _count; }
    Stats stats() const { return _stats; }
private:
    static constexpr int CACHE_SLOTS = 24;
    struct CachedTile { uint8_t *data = nullptr; uint32_t entry = UINT32_MAX; uint64_t age = 0; };
    FILE *_file = nullptr;
    uint8_t *_index = nullptr, *_frame = nullptr, *_scratch = nullptr;
    uint32_t _count = 0, _zooms = 0;
    uint8_t _min = 0, _max = 0;
    double _bounds[4] = {};
    int _width = 0, _height = 0;
    CachedTile _cache[CACHE_SLOTS];
    uint64_t _clock = 0;
    Error _error = Error::None;
    Stats _stats;
    bool fail(Error error) { _error = error; return false; }
    const uint8_t *tile(uint8_t zoom, uint32_t x, uint32_t y);
};
