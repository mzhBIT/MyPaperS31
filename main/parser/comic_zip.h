#pragma once
#include "memory/MetadataAllocator.h"
#include "parser/epub_zip.h"
#include <string>
#include <vector>
#include <unordered_map>

struct ComicPage {
    metadata_memory::String href;
    metadata_memory::String image_href;
    metadata_memory::String title;
};

class ComicZip {
public:
    bool open(const char *path);
    void close();
    const metadata_memory::Vector<ComicPage> &pages() const { return pages_; }
    const std::string &last_error() const { return error_; }
    bool read_page(const std::string &href, std::string *out) const;
    bool read_prefix(const std::string &href, std::vector<uint8_t> *out, size_t size);
    bool read_resource(const std::string &href, std::vector<uint8_t> *out, uint32_t size);
    bool extract_resource(const std::string &href, const std::string &path);
private:
    EpubZip zip_;
    metadata_memory::Vector<ComicPage> pages_;
    metadata_memory::Map<metadata_memory::String> resources_;
    std::string error_;
};
