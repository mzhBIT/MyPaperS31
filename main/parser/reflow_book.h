#pragma once

#include "parser/epub_parser.h"
#include "parser/fb2_parser.h"
#include "parser/docx_parser.h"
#include "parser/comic_zip.h"
#include "parser/mobi_parser.h"
#include "weread/WereadApi.h"

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
    enum class Format { None, Epub, Mobi, Fb2, Markdown, Weread, Docx, ComicZip };

    std::string title;

    bool open(const char *path);
    void close();
    Format format() const { return _format; }
    bool is_mobi() const { return _format == Format::Mobi; }
    bool is_fb2() const { return _format == Format::Fb2; }
    bool is_docx() const { return _format == Format::Docx; }
    bool is_comic_zip() const { return _format == Format::ComicZip; }
    bool is_markdown() const { return _format == Format::Markdown; }
    bool is_weread() const { return _format == Format::Weread; }
    const char *type_name() const {
        if (is_docx()) return "docx";
        if (is_comic_zip()) return "comic";
        return is_weread() ? "weread" : (is_mobi() ? "mobi" : (is_fb2() ? "fb2" : (is_markdown() ? "markdown" : "epub")));
    }
    const char *display_name() const {
        if (is_docx()) return "DOCX";
        if (is_comic_zip()) return "ZIP/CBZ";
        return is_weread() ? "WeRead" : (is_mobi() ? "MOBI" : (is_fb2() ? "FB2" : (is_markdown() ? "Markdown" : "EPUB")));
    }
    const std::string &last_error() const { return _last_error; }

    int spine_count() const { return static_cast<int>(_sections.size()); }
    const ReflowSectionItem &spine_item(int index) const { return _sections[static_cast<size_t>(index)]; }
    int toc_count() const { return static_cast<int>(_toc.size()); }
    const ReflowTocItem &toc_item(int index) const { return _toc[static_cast<size_t>(index)]; }
    const std::string &path() const { return _path; }
    const std::string &cover_image_href() const { return _cover_href; }
    const std::string &weread_book_id() const { return _weread_book.id; }
    const std::vector<weread::Chapter> &weread_chapters() const { return _weread_chapters; }
    const weread::Chapter *weread_chapter(int index) const {
        return index >= 0 && index < static_cast<int>(_weread_chapters.size())
                   ? &_weread_chapters[static_cast<size_t>(index)] : nullptr;
    }

    bool read_item_to_string(const std::string &href, std::string *out);
    bool read_kf8_flow(uint32_t flow_index, std::string *out) const;
    bool read_resource_prefix(const std::string &href, std::vector<uint8_t> *out,
                              size_t max_bytes = 64 * 1024);
    bool read_resource_to_memory(const std::string &href, std::vector<uint8_t> *out,
                                 uint32_t max_size = 12 * 1024 * 1024);
    bool resolve_footnote(const std::string &target, std::string *out);
    std::string normalize_resource_href(const std::string &href) const;
    bool extract_item_to_file(const std::string &href, const std::string &out_path);

private:
    Format _format = Format::None;
    std::string _path;
    std::string _cover_href;
    std::string _last_error;
    EpubBook _epub;
    MobiBook _mobi;
    Fb2Book _fb2;
    DocxBook _docx;
    ComicZip _comic;
    EpubBook _weread_epub;
    weread::Book _weread_book;
    std::vector<weread::Chapter> _weread_chapters;
    int _weread_active_chapter = -1;
    std::vector<ReflowSectionItem> _sections;
    std::vector<ReflowTocItem> _toc;
};
