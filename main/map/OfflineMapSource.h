#pragma once
#include "map/OfflineMap.h"
#include <string>

// A single legacy pack or a catalog with on-demand regional pack selection.
class OfflineMapSource {
public:
    ~OfflineMapSource() { close(); }
    bool open(const char *path);
    void close();
    // Catalogs are open before any regional pack (and its tiles) is loaded.
    bool is_open() const { return _opened; }
    bool render(double lon, double lat, uint8_t zoom, int width, int height);
    const uint8_t *frame() const { return _reader.frame(); }
    OfflineMap::Error error() const { return _error; }
    OfflineMap::Stats stats() const { return _reader.stats(); }
    bool supports(uint8_t z) const { return z <= 22 && (_zooms & (1u<<z)); }
    uint8_t min_zoom() const { return _min; }
    uint8_t max_zoom() const { return _max; }
    uint8_t default_zoom() const { return _default; }
    double center_lon() const { return _lon; }
    double center_lat() const { return _lat; }
    uint32_t count() const { return _reader.count(); }
    const char *active_path() const { return _active_path.c_str(); }
private:
    OfflineMap _reader;
    bool _opened = false;
    uint8_t *_records = nullptr;
    uint32_t _count = 0, _zooms = 0;
    int _active = -1;
    uint8_t _min = 0, _max = 0, _default = 0;
    double _lon = 0, _lat = 0;
    std::string _directory, _active_path;
    OfflineMap::Error _error = OfflineMap::Error::None;
    bool fail(OfflineMap::Error e) { _error = e; return false; }
};
