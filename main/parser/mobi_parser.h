#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct MobiSectionItem {
    std::string id;
    std::string href;
    std::string media_type = "text/html";
    uint32_t uncompressed_size = 0;
    uint64_t start = 0;
    uint64_t end = 0;
};

struct MobiTocItem {
    std::string title;
    std::string href;
    int spine_index = -1;
    int depth = 0;
};

class MobiBook {
public:
    std::string title;
    std::string author;

    bool open(const char *path);
    void close();

    int section_count() const { return static_cast<int>(_sections.size()); }
    const MobiSectionItem &section_item(int index) const { return _sections[static_cast<size_t>(index)]; }
    int toc_count() const { return static_cast<int>(_toc.size()); }
    const MobiTocItem &toc_item(int index) const { return _toc[static_cast<size_t>(index)]; }
    const std::string &path() const { return _path; }
    const std::string &cover_image_href() const { return _cover_href; }
    const std::string &last_error() const { return _last_error; }
    uint64_t text_length() const { return _decoded_text_length; }

    bool read_section_to_string(const std::string &href, std::string *out) const;
    bool read_kf8_flow(uint32_t flow_index, std::string *out) const;
    bool resolve_filepos_footnote(uint64_t position, std::string *out) const;
    std::string normalize_resource_href(const std::string &href) const;
    bool read_resource_prefix(const std::string &href, std::vector<uint8_t> *out,
                              size_t max_bytes) const;
    bool read_resource_to_memory(const std::string &href, std::vector<uint8_t> *out,
                                 uint32_t max_size) const;
    bool extract_resource_to_file(const std::string &href, const std::string &out_path) const;

private:
    struct HuffState;
    struct Record {
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    struct TextRecord {
        uint16_t record_index = 0;
        uint64_t start = 0;
        uint32_t length = 0;
    };

    struct Kf8Fragment {
        uint32_t insert_offset = 0;
        uint64_t raw_start = 0;
        uint32_t length = 0;
    };

    struct Kf8Part {
        uint64_t skeleton_start = 0;
        uint32_t skeleton_length = 0;
        uint32_t assembled_length = 0;
        std::vector<Kf8Fragment> fragments;
    };

    std::string _path;
    FILE *_file = nullptr;
    std::string _cover_href;
    std::string _last_error;
    std::vector<Record> _records;
    std::vector<TextRecord> _text_records;
    std::vector<MobiSectionItem> _sections;
    std::vector<MobiTocItem> _toc;
    uint16_t _compression = 0;
    uint16_t _encryption = 0;
    uint16_t _text_record_count = 0;
    uint16_t _extra_data_flags = 0;
    uint32_t _encoding = 65001;
    uint32_t _mobi_version = 0;
    bool _is_kf8 = false;
    uint32_t _fdst_record_index = 0xffffffffu;
    uint32_t _fdst_flow_count = 0;
    uint32_t _fragment_index = 0xffffffffu;
    uint32_t _skeleton_index = 0xffffffffu;
    uint32_t _first_image_record = 0xffffffffu;
    uint32_t _huff_record_index = 0xffffffffu;
    uint32_t _huff_record_count = 0;
    uint64_t _decoded_text_length = 0;
    std::vector<Kf8Part> _kf8_parts;
    std::shared_ptr<HuffState> _huff;

    bool parse_container();
    bool parse_record_zero(const std::vector<uint8_t> &record);
    bool build_text_index();
    bool build_kf8_index();
    bool read_kf8_part(size_t index, std::string *out) const;
    bool load_huff_dictionary();
    bool load_metadata_cache();
    void save_metadata_cache() const;
    std::string metadata_cache_path() const;
    bool read_record(size_t index, std::vector<uint8_t> *out) const;
    bool decode_text_record(size_t index, std::string *out) const;
    bool read_text_range(uint64_t start, uint64_t end, std::string *out) const;
    std::string resource_href(uint32_t resource_index) const;
};
