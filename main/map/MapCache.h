#pragma once
#include "map/AmapConfig.h"
#include <cstddef>
#include <string>
#include <utility>
#include <utility>

// Online static-map images only. Call from the UI after EPD/network work is idle.
// The directory must already exist; no recursive traversal or deletion is used.
class MapCache {
public:
    struct Usage { size_t entries = 0, bytes = 0; };
    explicit MapCache(std::string directory, size_t limit = 32u*1024*1024,
                      size_t entries = 128) : _directory(std::move(directory)), _limit(limit), _entries(entries) {}
    static std::string key(const AmapConfig &config, int width, int height, int server_scale);
    bool lookup(const std::string &key, std::string *path);
    bool store(const std::string &key, const char *source, std::string *path);
    bool invalidate(const std::string &key);
    bool clear();
    Usage usage() const;
private:
    std::string _directory;
    size_t _limit, _entries;
};
