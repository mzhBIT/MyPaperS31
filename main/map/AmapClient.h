#pragma once

#include "map/AmapConfig.h"

#include <string>
#include <atomic>
#include <vector>
#include <string_view>
#include <functional>

struct AmapRoute;
enum class AmapTravelMode;

struct AmapPlace {
    std::string name, address;
    double longitude = 0, latitude = 0; // GCJ-02, as returned by AMap.
};
struct AmapSearchResult {
    bool success = false;
    std::vector<AmapPlace> places;
    std::string error;
};

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
    AmapRoute plan_route(const AmapConfig &config, const AmapPlace &origin,
                        const AmapPlace &destination, AmapTravelMode mode,
                        const std::atomic<bool> *cancel = nullptr);
    static AmapSearchResult parse_search_response(std::string_view body);
    static AmapSearchResult parse_location_response(std::string_view body, bool ip,
                                                     double longitude, double latitude);
    AmapSearchResult lookup_location(const AmapConfig &config, bool ip,
                                    const std::atomic<bool> *cancel = nullptr);
    AmapSearchResult search_places(const AmapConfig &config, const std::string &keyword,
                                   const std::atomic<bool> *cancel = nullptr);

private:
    bool fetch_json(const std::string &url, size_t capacity, const std::atomic<bool> *cancel,
                    const std::function<void(std::string_view)> &parse, std::string &error);
    AmapSearchResult fetch_place_json(const AmapConfig &config, const std::string &url,
                                     int kind, const std::atomic<bool> *cancel);
    void *_client = nullptr;
};
