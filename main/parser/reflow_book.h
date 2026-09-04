#pragma once

#include "parser/epub_parser.h"
#include "parser/mobi_parser.h"

#include <cstdint>
#include <string>
#include <vector>

struct ReflowSectionItem {
    std::string id;
    std::string href;
    std::string media_type;
    uint32_t uncompressed_size = 0;
};

struct ReflowTocItem {
    std::string title;
    std::string href;
    int spine_index = -1;
    int depth = 0;
};

class ReflowBook {
public:
    enum class Format { None, Epub, Mobi };

    std::string title;

    bool open(const char *path);
    void close();
    Format format() const { return _format; }
    bool is_mobi() const { return _format == Format::Mobi; }
    const char *type_name() const { return is_mobi() ? "mobi" : "epub"; }
    const char *display_name() const { return is_mobi() ? "MOBI" : "EPUB"; }
    const std::string &last_error() const { return _last_error; }

    int spine_count() const { return static_cast<int>(_sections.size()); }
    const ReflowSectionItem &spine_item(int index) const { return _sections[static_cast<size_t>(index)]; }
    int toc_count() const { return static_cast<int>(_toc.size()); }
    const ReflowTocItem &toc_item(int index) const { return _toc[static_cast<size_t>(index)]; }
    const std::string &path() const { return _path; }
    const std::string &cover_image_href() const { return _cover_href; }

    bool read_item_to_string(const std::string &href, std::string *out);
    bool extract_item_to_file(const std::string &href, const std::string &out_path);

private:
    Format _format = Format::None;
    std::string _path;
    std::string _cover_href;
    std::string _last_error;
    EpubBook _epub;
    MobiBook _mobi;
    std::vector<ReflowSectionItem> _sections;
    std::vector<ReflowTocItem> _toc;
};
