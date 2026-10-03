#pragma once

#include "map/AmapConfig.h"

#include <string>
#include <atomic>

enum class AmapImageFormat {
    Unknown = 0,
    Jpeg,
    Png,
};

struct AmapMapResult {
    bool success = false;
    int http_status = 0;
    size_t bytes = 0;
    AmapImageFormat format = AmapImageFormat::Unknown;
    std::string path;
    std::string error;
};

class AmapClient {
public:
    AmapClient() = default;
    ~AmapClient();

    AmapClient(const AmapClient &) = delete;
    AmapClient &operator=(const AmapClient &) = delete;

    static AmapImageFormat probe_image(const char *path);
    AmapMapResult fetch_static_map(const AmapConfig &config,
                                   int output_width,
                                   int output_height,
                                   const std::atomic<bool> *cancel = nullptr);
    void close();

private:
    void *_client = nullptr;
};
