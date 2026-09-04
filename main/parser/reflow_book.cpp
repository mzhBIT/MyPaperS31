#include "parser/reflow_book.h"

#include <algorithm>
#include <cctype>

namespace {

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
    _format = Format::None;
    _path.clear();
    _cover_href.clear();
    _last_error.clear();
    title.clear();
    _sections.clear();
    _toc.clear();
}

bool ReflowBook::read_item_to_string(const std::string &href, std::string *out)
{
    if (_format == Format::Mobi) return _mobi.read_section_to_string(href, out);
    if (_format == Format::Epub) return _epub.read_item_to_string(href, out);
    return false;
}

bool ReflowBook::extract_item_to_file(const std::string &href, const std::string &out_path)
{
    if (_format == Format::Mobi) return _mobi.extract_resource_to_file(href, out_path);
    if (_format == Format::Epub) return _epub.extract_item_to_file(href, out_path);
    return false;
}
