#pragma once
#include "parser/epub_zip.h"
#include <string>
#include <vector>
#include <unordered_map>

struct ComicPage {
    std::string href;
    std::string image_href;
    std::string title;
};

class ComicZip {
public:
    bool open(const char *path);
    void close();
    const std::vector<ComicPage> &pages() const { return pages_; }
    const std::string &last_error() const { return error_; }
    bool read_page(const std::string &href, std::string *out) const;
    bool read_prefix(const std::string &href, std::vector<uint8_t> *out, size_t size);
    bool read_resource(const std::string &href, std::vector<uint8_t> *out, uint32_t size);
    bool extract_resource(const std::string &href, const std::string &path);
private:
    EpubZip zip_;
    std::vector<ComicPage> pages_;
    std::unordered_map<std::string, std::string> resources_;
    std::string error_;
};
