#pragma once
#include "memory/MetadataAllocator.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct Fb2SectionItem {
    metadata_memory::String id;
    metadata_memory::String href;
    metadata_memory::String title;
    uint64_t start = 0;
    uint64_t end = 0;
    bool in_spine = true;
};

class Fb2Book {
public:
    std::string title;
    std::string author;

    bool open(const char *path);
    void close();

    int section_count() const { return static_cast<int>(_spine_indices.size()); }
    const Fb2SectionItem &section_item(int index) const {
        return _sections[_spine_indices[static_cast<size_t>(index)]];
    }
    const std::string &cover_image_href() const { return _cover_href; }
    const std::string &last_error() const { return _last_error; }

    bool read_section_to_string(const std::string &href, std::string *out) const;
    bool read_resource_prefix(const std::string &href, std::vector<uint8_t> *out,
                              size_t max_bytes) const;
    bool read_resource_to_memory(const std::string &href, std::vector<uint8_t> *out,
                                 uint32_t max_size) const;
    bool extract_resource_to_file(const std::string &href, const std::string &out_path) const;

private:
    struct BinaryItem {
        metadata_memory::String id;
        metadata_memory::String href;
        metadata_memory::String media_type;
        uint64_t start = 0;
        uint64_t end = 0;
    };

    std::string _path;
    std::string _cover_href;
    std::string _last_error;
    metadata_memory::Vector<Fb2SectionItem> _sections;
    metadata_memory::Vector<size_t> _spine_indices;
    metadata_memory::Vector<BinaryItem> _binaries;
    metadata_memory::Map<metadata_memory::String> _anchor_hrefs;
    metadata_memory::Map<metadata_memory::String> _binary_hrefs;

    bool read_range(uint64_t start, uint64_t end, std::string *out) const;
    bool decode_resource(const std::string &href, std::vector<uint8_t> *out,
                         size_t max_bytes, bool allow_prefix) const;
    std::string transform_section(const Fb2SectionItem &section,
                                  const std::string &xml) const;
};
