#pragma once
#include "map/AmapConfig.h"
#include <cstddef>
#include <string>
#include <cstdint>
#include <vector>
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
    // Consume a closed, complete downloader-owned temporary file on this SD card.
    // CRC must have been computed over all successfully written bytes.
    // On pre-commit failure the source is retained for the caller's fallback.
    bool store_download(const std::string &key, const char *source, size_t bytes,
                        uint32_t crc, std::string *path);
    void reset_index() { _index.clear(); _index_ready=false; _age=0; }
    bool invalidate(const std::string &key);
    bool clear();
    Usage usage() const;
private:
    struct CachedEntry { std::string key; uint32_t bytes, crc; uint64_t age; };
    bool ensure_index();
    bool store_impl(const std::string &key, const char *source, size_t bytes,
                    uint32_t crc, bool adopt, std::string *path);
    std::vector<CachedEntry> _index;
    bool _index_ready = false;
    uint64_t _age = 0;
    std::string _directory;
    size_t _limit, _entries;
};
