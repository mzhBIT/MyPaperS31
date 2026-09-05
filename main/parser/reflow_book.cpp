#include "parser/reflow_book.h"
#include "parser/epub_html.h"
#include "weread/WereadContentSource.h"
#include "weread/WereadStore.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace {

bool regular_file_exists(const std::string &path)
{
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

std::string lower_extension(const char *path)
{
    if (!path) return std::string();
    std::string value(path);
    size_t dot = value.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    value.erase(0, dot);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

bool ReflowBook::open(const char *path)
{
    close();
    if (!path || !path[0]) return false;
    _path = path;
    std::string ext = lower_extension(path);
    if (ext == ".wrbook") {
        std::string source_format;
        if (!weread::store::load_manifest(path, &_weread_book, &_weread_chapters, &source_format)) {
            _last_error = "Could not parse WeRead manifest";
            return false;
        }
        _format = Format::Weread;
        title = _weread_book.title;
        const std::string cached_cover = weread::store::cover_path(_weread_book.cover_url);
        _cover_href = cached_cover;

        const std::string epub_path = weread::store::epub_payload_path(_weread_book.id);
        const bool packaged_epub = regular_file_exists(epub_path) && _weread_epub.open(epub_path.c_str());
        _sections.reserve(_weread_chapters.size());
        _toc.reserve(_weread_chapters.size());
        for (size_t i = 0; i < _weread_chapters.size(); ++i) {
            const auto &chapter = _weread_chapters[i];
            std::string href = "wrchapter/" + std::to_string(i) + ".xhtml";
            if (packaged_epub && _weread_epub.spine_count() > 0) {
                const int spine = std::clamp(chapter.index - 1, 0, _weread_epub.spine_count() - 1);
                href = _weread_epub.spine_item(spine).href;
            }
            _sections.push_back({chapter.uid, href, "application/xhtml+xml", 0});
            _toc.push_back({chapter.title, href, static_cast<int>(i), 0});
        }
        return !_sections.empty();
    }
    if (ext == ".mobi" || ext == ".prc") {
        if (!_mobi.open(path)) {
            _last_error = _mobi.last_error();
            return false;
        }
        _format = Format::Mobi;
        title = _mobi.title;
        _cover_href = _mobi.cover_image_href();
        _sections.reserve(_mobi.section_count());
        for (int i = 0; i < _mobi.section_count(); ++i) {
            const auto &source = _mobi.section_item(i);
            _sections.push_back({source.id, source.href, source.media_type, source.uncompressed_size});
        }
        _toc.reserve(_mobi.toc_count());
        for (int i = 0; i < _mobi.toc_count(); ++i) {
            const auto &source = _mobi.toc_item(i);
            _toc.push_back({source.title, source.href, source.spine_index, source.depth});
        }
        return true;
    }
    if (ext == ".fb2") {
        if (!_fb2.open(path)) {
            _last_error = _fb2.last_error();
            return false;
        }
        _format = Format::Fb2;
        title = _fb2.title;
        _cover_href = _fb2.cover_image_href();
        _sections.reserve(_fb2.section_count());
        _toc.reserve(_fb2.section_count());
        for (int i = 0; i < _fb2.section_count(); ++i) {
            const auto &source = _fb2.section_item(i);
            const uint32_t size = source.end > source.start
                ? static_cast<uint32_t>(std::min<uint64_t>(source.end - source.start, UINT32_MAX)) : 0;
            _sections.push_back({source.id, source.href, "application/xhtml+xml", size});
            _toc.push_back({source.title, source.href, i, 0});
        }
        return true;
    }

    if (!_epub.open(path)) {
        _last_error = "Could not parse EPUB container/OPF";
        return false;
    }
    _format = Format::Epub;
    title = _epub.title;
    _cover_href = _epub.cover_image_href();
    _sections.reserve(_epub.spine_count());
    for (int i = 0; i < _epub.spine_count(); ++i) {
        const auto &source = _epub.spine_item(i);
        _sections.push_back({source.id, source.href, source.media_type, source.uncompressed_size});
    }
    _toc.reserve(_epub.toc_count());
    for (int i = 0; i < _epub.toc_count(); ++i) {
        const auto &source = _epub.toc_item(i);
        _toc.push_back({source.title, source.href, source.spine_index, source.depth});
    }
    return true;
}

void ReflowBook::close()
{
    _epub.close();
    _mobi.close();
    _fb2.close();
    _weread_epub.close();
    _format = Format::None;
    _path.clear();
    _cover_href.clear();
    _last_error.clear();
    title.clear();
    _sections.clear();
    _toc.clear();
    _weread_book = {};
    _weread_chapters.clear();
    _weread_active_chapter = -1;
}

bool ReflowBook::read_item_to_string(const std::string &href, std::string *out)
{
    if (_format == Format::Mobi) return _mobi.read_section_to_string(href, out);
    if (_format == Format::Fb2) return _fb2.read_section_to_string(href, out);
    if (_format == Format::Epub) return _epub.read_item_to_string(href, out);
    if (_format == Format::Weread) {
        if (_weread_epub.spine_count() > 0) return _weread_epub.read_item_to_string(href, out);
        const std::string prefix = "wrchapter/";
        if (href.rfind(prefix, 0) != 0) return false;
        char *end = nullptr;
        const long index = std::strtol(href.c_str() + prefix.size(), &end, 10);
        if (end == href.c_str() + prefix.size() || index < 0 ||
            index >= static_cast<long>(_weread_chapters.size())) return false;
        _weread_active_chapter = static_cast<int>(index);
        std::string error;
        const bool ok = weread::load_chapter_content(_weread_book.id,
                                                      _weread_chapters[static_cast<size_t>(index)],
                                                      out, &error);
        if (!ok) _last_error = std::move(error);
        return ok;
    }
    return false;
}

bool ReflowBook::resolve_footnote(const std::string &target, std::string *out)
{
    if (!out) return false;
    const size_t hash = target.find('#');
    if (hash == std::string::npos || hash == 0 || hash + 1 >= target.size()) return false;
    const std::string fragment = target.substr(hash + 1);
    static constexpr const char *MOBI_FILEPOS = "mobi-filepos-";
    if (_format == Format::Mobi && fragment.rfind(MOBI_FILEPOS, 0) == 0) {
        char *end = nullptr;
        const uint64_t position = std::strtoull(fragment.c_str() + std::strlen(MOBI_FILEPOS), &end, 10);
        if (end && *end == '\0') return _mobi.resolve_filepos_footnote(position, out);
        return false;
    }

    const std::string href = target.substr(0, hash);
    std::string xhtml;
    return read_item_to_string(href, &xhtml) &&
           epub_resolve_footnote(xhtml, href, target, out);
}

bool ReflowBook::extract_item_to_file(const std::string &href, const std::string &out_path)
{
    if (_format == Format::Mobi) return _mobi.extract_resource_to_file(href, out_path);
    if (_format == Format::Fb2) return _fb2.extract_resource_to_file(href, out_path);
    if (_format == Format::Epub) return _epub.extract_item_to_file(href, out_path);
    if (_format == Format::Weread) {
        if (href == _cover_href) {
            FILE *source = std::fopen(href.c_str(), "rb");
            FILE *target = source ? std::fopen(out_path.c_str(), "wb") : nullptr;
            bool ok = source && target;
            char buffer[4096];
            while (ok) {
                const size_t count = std::fread(buffer, 1, sizeof(buffer), source);
                if (count && std::fwrite(buffer, 1, count, target) != count) ok = false;
                if (count < sizeof(buffer)) { if (std::ferror(source)) ok = false; break; }
            }
            if (target) std::fclose(target);
            if (source) std::fclose(source);
            return ok;
        }
        if (_weread_epub.spine_count() > 0) return _weread_epub.extract_item_to_file(href, out_path);
        const weread::Chapter *chapter = _weread_active_chapter >= 0 &&
                _weread_active_chapter < static_cast<int>(_weread_chapters.size())
            ? &_weread_chapters[static_cast<size_t>(_weread_active_chapter)] : nullptr;
        return weread::extract_resource(_weread_book.id, chapter, href, out_path);
    }
    return false;
}
