#pragma once

#include "parser/epub_zip.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct EpubManifestItem {
    std::string id;
    std::string href;
    std::string media_type;
    std::string properties;
};

struct EpubSpineItem {
    std::string id;
    std::string href;
    std::string media_type;
    uint32_t uncompressed_size = 0;
};

struct EpubTocItem {
    std::string title;
    std::string href;
    int spine_index = -1;
    int depth = 0;
};

class EpubBook {
public:
    std::string title;

    bool open(const char *path);
    void close();
    int spine_count() const { return (int)_spine.size(); }
    const EpubSpineItem &spine_item(int index) const { return _spine[(size_t)index]; }
    const std::string &opf_path() const { return _opf_path; }
    const std::string &path() const { return _path; }
    const std::string &cover_image_href() const { return _cover_image_href; }
    int toc_count() const { return (int)_toc.size(); }
    const EpubTocItem &toc_item(int index) const { return _toc[(size_t)index]; }
    static bool cached_progress_info(const char *path, uint32_t spine, uint64_t position,
                                     int *percent, int *chapter, int *chapter_count);
    // Homepage metadata only: container + bounded OPF, no chapter/TOC layout.
    static bool brief_metadata(const char *path,std::string *title,std::string *author);
    bool read_item_to_string(const std::string &href, std::string *out);
    bool read_item_prefix(const std::string &href, std::vector<uint8_t> *out,
                          size_t max_bytes = 64 * 1024);
    bool read_item_to_memory(const std::string &href, std::vector<uint8_t> *out,
                             uint32_t max_size = 12 * 1024 * 1024);
    bool extract_item_to_file(const std::string &href, const std::string &out_path);

private:
    std::string _path;
    std::string _opf_path;
    std::string _opf_base;
    std::string _ncx_path;
    std::string _nav_path;
    std::string _cover_image_href;
    EpubZip _zip;
    std::unordered_map<std::string, EpubManifestItem> _manifest_by_id;
    std::unordered_map<std::string, int> _spine_index_by_href;
    std::vector<EpubSpineItem> _spine;
    std::vector<EpubTocItem> _toc;

    bool parse_container(const std::string &xml);
    bool parse_opf(const std::string &xml);
    void load_toc();
    bool parse_ncx_toc(const std::string &xml, const std::string &toc_path);
    bool parse_nav_toc(const std::string &xhtml, const std::string &toc_path);
    bool load_metadata_cache(const char *source_path);
    void save_metadata_cache(const char *source_path) const;
    static std::string metadata_cache_path(const char *source_path);
    std::string resolve_from_opf(const std::string &href) const;
    void rebuild_spine_index();
    int spine_index_for_href(const std::string &href) const;
};
