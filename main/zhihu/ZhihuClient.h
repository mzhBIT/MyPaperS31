#pragma once

#include "zhihu/ZhihuConfig.h"

#include <string>
#include <vector>

struct ZhihuHotItem {
    std::string title;
    std::string url;
    std::string thumbnail_url;
    std::string summary;
};

struct ZhihuHotListResult {
    bool success = false;
    bool from_cache = false;
    int http_status = 0;
    int api_code = 0;
    std::string error;
    std::vector<ZhihuHotItem> items;
};

struct ZhihuContentResult {
    bool success = false;
    int http_status = 0;
    std::string text;
    std::string error;
};

class ZhihuClient {
public:
    static ZhihuHotListResult fetch_hot_list(const ZhihuConfig &config, int limit = 30,
                                             bool force_network = false);
    static ZhihuContentResult fetch_content(const std::string &url);
};
