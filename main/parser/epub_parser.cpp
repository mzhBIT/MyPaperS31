#include "parser/epub_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include <vector>

#include "storage/app_paths.h"

static const char *TAG = "EPUB";
static const uint32_t EPUB_META_MAGIC = 0x4154454D; // META
static const uint32_t EPUB_META_VERSION = 1;
static const uint32_t EPUB_META_MAX_BYTES = 8 * 1024 * 1024;

static uint32_t meta_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t meta_rd64(const uint8_t *p)
{
    return (uint64_t)meta_rd32(p) | ((uint64_t)meta_rd32(p + 4) << 32);
}

static void meta_append32(std::vector<uint8_t> *out, uint32_t value)
{
    out->push_back((uint8_t)value);
    out->push_back((uint8_t)(value >> 8));
    out->push_back((uint8_t)(value >> 16));
    out->push_back((uint8_t)(value >> 24));
}

static void meta_append64(std::vector<uint8_t> *out, uint64_t value)
{
    meta_append32(out, (uint32_t)value);
    meta_append32(out, (uint32_t)(value >> 32));
}

static bool meta_append_string(std::vector<uint8_t> *out, const std::string &value)
{
    if (value.size() > 1024 * 1024) return false;
    meta_append32(out, (uint32_t)value.size());
    out->insert(out->end(), value.begin(), value.end());
    return true;
}

static uint64_t meta_path_hash64(const char *path)
{
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t *p = (const uint8_t *)(path ? path : ""); *p; ++p) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class MetaReader {
public:
    explicit MetaReader(const std::vector<uint8_t> &data) : _data(data) {}

    bool u32(uint32_t *value)
    {
        if (!value || _pos + 4 > _data.size()) return false;
        *value = meta_rd32(_data.data() + _pos);
        _pos += 4;
        return true;
    }

    bool u64(uint64_t *value)
    {
        if (!value || _pos + 8 > _data.size()) return false;
        *value = meta_rd64(_data.data() + _pos);
        _pos += 8;
        return true;
    }

    bool string(std::string *value)
    {
        uint32_t length = 0;
        if (!value || !u32(&length) || length > 1024 * 1024 || _pos + length > _data.size()) return false;
        value->assign((const char *)_data.data() + _pos, length);
        _pos += length;
        return true;
    }

    bool done() const { return _pos == _data.size(); }

private:
    const std::vector<uint8_t> &_data;
    size_t _pos = 0;
};

static std::string trim_copy(const std::string &s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string local_name(std::string name)
{
    size_t p = name.find(':');
    if (p != std::string::npos) name.erase(0, p + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return name;
}

static bool parse_tag_attrs(const std::string &tag, std::string *name, std::unordered_map<std::string, std::string> *attrs)
{
    if (!name || !attrs) return false;
    attrs->clear();

    size_t pos = 0;
    while (pos < tag.size() && std::isspace((unsigned char)tag[pos])) pos++;
    if (pos < tag.size() && tag[pos] == '/') pos++;
    while (pos < tag.size() && std::isspace((unsigned char)tag[pos])) pos++;

    size_t start = pos;
    while (pos < tag.size() && !std::isspace((unsigned char)tag[pos]) && tag[pos] != '/' && tag[pos] != '>') pos++;
    if (pos == start) return false;
    *name = local_name(tag.substr(start, pos - start));

    while (pos < tag.size()) {
        while (pos < tag.size() && (std::isspace((unsigned char)tag[pos]) || tag[pos] == '/')) pos++;
        if (pos >= tag.size()) break;

        size_t key_start = pos;
        while (pos < tag.size() && tag[pos] != '=' && !std::isspace((unsigned char)tag[pos])) pos++;
        std::string key = local_name(tag.substr(key_start, pos - key_start));
        while (pos < tag.size() && std::isspace((unsigned char)tag[pos])) pos++;
        if (pos >= tag.size() || tag[pos] != '=') {
            (*attrs)[key] = "";
            continue;
        }
        pos++;
        while (pos < tag.size() && std::isspace((unsigned char)tag[pos])) pos++;
        if (pos >= tag.size()) break;

        char quote = tag[pos];
        std::string value;
        if (quote == '"' || quote == '\'') {
            pos++;
            size_t value_start = pos;
            while (pos < tag.size() && tag[pos] != quote) pos++;
            value = tag.substr(value_start, pos - value_start);
            if (pos < tag.size()) pos++;
        } else {
            size_t value_start = pos;
            while (pos < tag.size() && !std::isspace((unsigned char)tag[pos]) && tag[pos] != '/') pos++;
            value = tag.substr(value_start, pos - value_start);
        }
        (*attrs)[key] = value;
    }

    return true;
}

static std::string attr_value(const std::unordered_map<std::string, std::string> &attrs, const char *key)
{
    auto it = attrs.find(key);
    return it == attrs.end() ? std::string() : it->second;
}

static std::string dirname_of(const std::string &path)
{
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string() : path.substr(0, p + 1);
}

static std::string normalize_join(const std::string &base, const std::string &href)
{
    std::string path = href;
    size_t hash = path.find('#');
    if (hash != std::string::npos) path.erase(hash);
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path[0] == '/') path.erase(path.begin());
    else path = base + path;

    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (part.empty() || part == ".") {
        } else if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }

    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

static std::string decode_entities(std::string s)
{
    struct Named {
        const char *from;
        const char *to;
    };
    static const Named named[] = {
        {"&amp;", "&"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&nbsp;", " "},
    };
    for (const auto &n : named) {
        size_t pos = 0;
        while ((pos = s.find(n.from, pos)) != std::string::npos) {
            s.replace(pos, std::strlen(n.from), n.to);
            pos += std::strlen(n.to);
        }
    }
    return s;
}

static bool contains_word(const std::string &list, const char *word)
{
    size_t pos = 0;
    size_t len = std::strlen(word);
    while ((pos = list.find(word, pos)) != std::string::npos) {
        bool left = pos == 0 || std::isspace((unsigned char)list[pos - 1]);
        bool right = pos + len >= list.size() || std::isspace((unsigned char)list[pos + len]);
        if (left && right) return true;
        pos += len;
    }
    return false;
}

static bool has_image_href(const std::string &href)
{
    std::string s = href;
    size_t q = s.find_first_of("?#");
    if (q != std::string::npos) s.erase(q);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s.size() >= 4 &&
           (s.rfind(".jpg") == s.size() - 4 ||
            s.rfind(".png") == s.size() - 4 ||
            s.rfind(".bmp") == s.size() - 4 ||
            (s.size() >= 5 && s.rfind(".jpeg") == s.size() - 5));
}

static bool tag_has_closing_slash(const std::string &tag)
{
    size_t pos = tag.size();
    while (pos > 0 && std::isspace((unsigned char)tag[pos - 1])) pos--;
    return pos > 0 && tag[pos - 1] == '/';
}

static std::string strip_fragment(std::string href)
{
    size_t hash = href.find('#');
    if (hash != std::string::npos) href.erase(hash);
    return href;
}

bool EpubBook::open(const char *path)
{
    int64_t t0 = esp_timer_get_time();
    close();
    if (!_zip.open(path)) return false;
    _path = path ? path : "";

    if (load_metadata_cache(path)) {
        ESP_LOGI(TAG, "opened cached title=%s spine=%d toc=%d time=%lld ms",
                 title.c_str(), spine_count(), toc_count(),
                 (esp_timer_get_time() - t0) / 1000);
        return true;
    }

    std::string container;
    if (!_zip.read_entry_to_string("META-INF/container.xml", &container, true)) {
        ESP_LOGE(TAG, "container.xml missing");
        close();
        return false;
    }
    if (!parse_container(container) || _opf_path.empty()) {
        ESP_LOGE(TAG, "container.xml parse failed");
        close();
        return false;
    }

    std::string opf;
    if (!_zip.read_entry_to_string(_opf_path, &opf, true)) {
        ESP_LOGE(TAG, "opf read failed: %s", _opf_path.c_str());
        close();
        return false;
    }
    _opf_base = dirname_of(_opf_path);
    if (!parse_opf(opf) || _spine.empty()) {
        ESP_LOGE(TAG, "opf parse failed: %s", _opf_path.c_str());
        close();
        return false;
    }
    load_toc();

    if (title.empty()) title = _path;
    save_metadata_cache(path);
    ESP_LOGI(TAG, "opened title=%s spine=%d toc=%d opf=%s time=%lld ms",
             title.c_str(), spine_count(), toc_count(), _opf_path.c_str(),
             (esp_timer_get_time() - t0) / 1000);
    return true;
}

void EpubBook::close()
{
    title.clear();
    _path.clear();
    _opf_path.clear();
    _opf_base.clear();
    _ncx_path.clear();
    _nav_path.clear();
    _cover_image_href.clear();
    _manifest_by_id.clear();
    _spine_index_by_href.clear();
    _spine.clear();
    _toc.clear();
    _zip.close();
}

bool EpubBook::parse_container(const std::string &xml)
{
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos + 1);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos + 1, end - pos - 1);
        std::string name;
        std::unordered_map<std::string, std::string> attrs;
        if (parse_tag_attrs(tag, &name, &attrs) && name == "rootfile") {
            _opf_path = attr_value(attrs, "full-path");
            return !_opf_path.empty();
        }
        pos = end + 1;
    }
    return false;
}

bool EpubBook::parse_opf(const std::string &xml)
{
    std::vector<std::string> spine_idrefs;
    std::string cover_item_id;
    std::string guide_cover_page_href;
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        if (pos + 1 < xml.size() && (xml[pos + 1] == '!' || xml[pos + 1] == '?')) {
            size_t skip = xml.find('>', pos + 1);
            if (skip == std::string::npos) break;
            pos = skip + 1;
            continue;
        }

        size_t end = xml.find('>', pos + 1);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos + 1, end - pos - 1);
        std::string name;
        std::unordered_map<std::string, std::string> attrs;
        if (!parse_tag_attrs(tag, &name, &attrs)) {
            pos = end + 1;
            continue;
        }

        if (name == "title" && title.empty()) {
            std::string close_tag = "</" + tag.substr(0, tag.find_first_of(" \t\r\n/>")) + ">";
            size_t title_end = xml.find(close_tag, end + 1);
            if (title_end == std::string::npos) title_end = xml.find("</dc:title>", end + 1);
            if (title_end != std::string::npos) {
                title = decode_entities(trim_copy(xml.substr(end + 1, title_end - end - 1)));
            }
        } else if (name == "meta") {
            if (attr_value(attrs, "name") == "cover") {
                cover_item_id = attr_value(attrs, "content");
            }
        } else if (name == "item") {
            EpubManifestItem item;
            item.id = attr_value(attrs, "id");
            item.href = resolve_from_opf(attr_value(attrs, "href"));
            item.media_type = attr_value(attrs, "media-type");
            item.properties = attr_value(attrs, "properties");
            if (!item.id.empty() && !item.href.empty()) {
                _manifest_by_id[item.id] = item;
                if (contains_word(item.properties, "nav")) _nav_path = item.href;
                if (item.id == cover_item_id || contains_word(item.properties, "cover-image")) {
                    _cover_image_href = item.href;
                }
                if (item.media_type == "application/x-dtbncx+xml") _ncx_path = item.href;
            }
        } else if (name == "itemref") {
            std::string idref = attr_value(attrs, "idref");
            if (!idref.empty()) spine_idrefs.push_back(idref);
        } else if (name == "reference") {
            std::string type = attr_value(attrs, "type");
            if ((type == "cover" || type == "cover-page") && guide_cover_page_href.empty()) {
                guide_cover_page_href = resolve_from_opf(attr_value(attrs, "href"));
            }
        }

        pos = end + 1;
    }

    if (_cover_image_href.empty() && !cover_item_id.empty()) {
        auto it = _manifest_by_id.find(cover_item_id);
        if (it != _manifest_by_id.end()) _cover_image_href = it->second.href;
    }

    if (_cover_image_href.empty() && !guide_cover_page_href.empty()) {
        std::string cover_page;
        if (_zip.read_entry_to_string(guide_cover_page_href, &cover_page, true)) {
            std::string base = dirname_of(guide_cover_page_href);
            const char *patterns[] = {"xlink:href=\"", "src=\"", "href=\"", "xlink:href='", "src='", "href='"};
            for (const char *pattern : patterns) {
                size_t p = cover_page.find(pattern);
                while (p != std::string::npos) {
                    p += std::strlen(pattern);
                    char quote = pattern[std::strlen(pattern) - 1];
                    size_t e = cover_page.find(quote, p);
                    if (e == std::string::npos) break;
                    std::string ref = cover_page.substr(p, e - p);
                    if (has_image_href(ref)) {
                        _cover_image_href = normalize_join(base, ref);
                        break;
                    }
                    p = cover_page.find(pattern, e + 1);
                }
                if (!_cover_image_href.empty()) break;
            }
        }
    }

    for (const auto &idref : spine_idrefs) {
        auto it = _manifest_by_id.find(idref);
        if (it == _manifest_by_id.end()) continue;
        const auto &mi = it->second;
        EpubSpineItem sp;
        sp.id = mi.id;
        sp.href = mi.href;
        sp.media_type = mi.media_type;
        EpubZip::Entry entry;
        if (_zip.entry_info(sp.href, &entry)) sp.uncompressed_size = entry.uncompressed_size;
        _spine.push_back(sp);
    }

    rebuild_spine_index();

    return !_spine.empty();
}

void EpubBook::load_toc()
{
    _toc.clear();

    std::string toc;
    if (!_nav_path.empty() && _zip.read_entry_to_string(_nav_path, &toc, true)) {
        if (parse_nav_toc(toc, _nav_path) && !_toc.empty()) {
            ESP_LOGI(TAG, "toc nav items=%d path=%s", toc_count(), _nav_path.c_str());
            return;
        }
    }

    if (!_ncx_path.empty() && _zip.read_entry_to_string(_ncx_path, &toc, true)) {
        if (parse_ncx_toc(toc, _ncx_path) && !_toc.empty()) {
            ESP_LOGI(TAG, "toc ncx items=%d path=%s", toc_count(), _ncx_path.c_str());
            return;
        }
    }

    ESP_LOGI(TAG, "toc unavailable nav=%s ncx=%s", _nav_path.c_str(), _ncx_path.c_str());
}

bool EpubBook::parse_ncx_toc(const std::string &xml, const std::string &toc_path)
{
    _toc.clear();
    std::string base = dirname_of(toc_path);
    size_t pos = 0;
    int depth = 0;

    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos + 1);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos + 1, end - pos - 1);
        std::string name;
        std::unordered_map<std::string, std::string> attrs;
        if (!parse_tag_attrs(tag, &name, &attrs)) {
            pos = end + 1;
            continue;
        }

        bool closing = !tag.empty() && tag[0] == '/';
        if (name == "navpoint") {
            if (closing) {
                if (depth > 0) depth--;
            } else {
                size_t close_pos = xml.find("</navPoint>", end + 1);
                if (close_pos == std::string::npos) close_pos = xml.find("</navpoint>", end + 1);
                size_t label_start = xml.find("<text", end + 1);
                size_t label_gt = label_start == std::string::npos ? std::string::npos : xml.find('>', label_start + 1);
                size_t label_end = label_gt == std::string::npos ? std::string::npos : xml.find("</text>", label_gt + 1);
                size_t content_start = xml.find("<content", end + 1);
                if (close_pos != std::string::npos) {
                    if (label_start > close_pos) label_start = std::string::npos;
                    if (content_start > close_pos) content_start = std::string::npos;
                }

                std::string label;
                std::string href;
                if (label_gt != std::string::npos && label_end != std::string::npos) {
                    label = decode_entities(trim_copy(xml.substr(label_gt + 1, label_end - label_gt - 1)));
                }
                if (content_start != std::string::npos) {
                    size_t content_gt = xml.find('>', content_start + 1);
                    if (content_gt != std::string::npos) {
                        std::string content_tag = xml.substr(content_start + 1, content_gt - content_start - 1);
                        std::string content_name;
                        std::unordered_map<std::string, std::string> content_attrs;
                        if (parse_tag_attrs(content_tag, &content_name, &content_attrs)) {
                            href = normalize_join(base, attr_value(content_attrs, "src"));
                        }
                    }
                }

                int spine = spine_index_for_href(href);
                if (!label.empty() && spine >= 0) {
                    EpubTocItem item;
                    item.title = label;
                    item.href = href;
                    item.spine_index = spine;
                    item.depth = depth;
                    _toc.push_back(item);
                }
                if (!tag_has_closing_slash(tag)) depth++;
            }
        }
        pos = end + 1;
    }

    return !_toc.empty();
}

bool EpubBook::parse_nav_toc(const std::string &xhtml, const std::string &toc_path)
{
    _toc.clear();
    std::string base = dirname_of(toc_path);
    bool in_nav = false;
    int nav_depth = 0;
    int list_depth = 0;
    size_t pos = 0;

    while ((pos = xhtml.find('<', pos)) != std::string::npos) {
        if (xhtml.compare(pos, 4, "<!--") == 0) {
            size_t comment_end = xhtml.find("-->", pos + 4);
            pos = comment_end == std::string::npos ? xhtml.size() : comment_end + 3;
            continue;
        }

        size_t end = xhtml.find('>', pos + 1);
        if (end == std::string::npos) break;
        std::string tag = xhtml.substr(pos + 1, end - pos - 1);
        std::string name;
        std::unordered_map<std::string, std::string> attrs;
        if (!parse_tag_attrs(tag, &name, &attrs)) {
            pos = end + 1;
            continue;
        }

        bool closing = !tag.empty() && tag[0] == '/';
        bool self_closing = tag_has_closing_slash(tag);
        if (name == "nav") {
            if (!closing) {
                std::string type = attr_value(attrs, "type");
                std::string epub_type = attr_value(attrs, "epub:type");
                in_nav = contains_word(type, "toc") || contains_word(epub_type, "toc") || type == "toc" || epub_type == "toc";
                nav_depth = in_nav && !self_closing ? 1 : 0;
            } else if (in_nav) {
                nav_depth--;
                if (nav_depth <= 0) in_nav = false;
            }
            pos = end + 1;
            continue;
        }

        if (!in_nav) {
            pos = end + 1;
            continue;
        }

        if (!closing && !self_closing) nav_depth++;
        if (name == "ol" || name == "ul") {
            if (closing) list_depth = std::max(0, list_depth - 1);
            else list_depth++;
        } else if (!closing && name == "a") {
            std::string href = normalize_join(base, attr_value(attrs, "href"));
            int spine = spine_index_for_href(href);
            size_t text_start = end + 1;
            size_t text_end = xhtml.find("</a>", text_start);
            if (text_end == std::string::npos) text_end = xhtml.find("</A>", text_start);
            if (text_end != std::string::npos && spine >= 0) {
                std::string raw = xhtml.substr(text_start, text_end - text_start);
                std::string label;
                bool in_tag = false;
                for (size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '<') {
                        in_tag = true;
                    } else if (raw[i] == '>') {
                        in_tag = false;
                    } else if (!in_tag) {
                        label.push_back(raw[i]);
                    }
                }
                label = decode_entities(trim_copy(label));
                if (!label.empty()) {
                    EpubTocItem item;
                    item.title = label;
                    item.href = href;
                    item.spine_index = spine;
                    item.depth = std::max(0, list_depth - 1);
                    _toc.push_back(item);
                }
            }
        }

        if (closing) nav_depth = std::max(0, nav_depth - 1);
        if (nav_depth <= 0) in_nav = false;
        pos = end + 1;
    }

    return !_toc.empty();
}

std::string EpubBook::resolve_from_opf(const std::string &href) const
{
    return normalize_join(_opf_base, href);
}

int EpubBook::spine_index_for_href(const std::string &href) const
{
    std::string target = strip_fragment(href);
    auto it = _spine_index_by_href.find(target);
    return it == _spine_index_by_href.end() ? -1 : it->second;
}

void EpubBook::rebuild_spine_index()
{
    _spine_index_by_href.clear();
    _spine_index_by_href.reserve(_spine.size());
    for (size_t i = 0; i < _spine.size(); ++i) {
        _spine_index_by_href.emplace(_spine[i].href, (int)i);
    }
}

std::string EpubBook::metadata_cache_path(const char *source_path)
{
    char name[48];
    snprintf(name, sizeof(name), "/%016llx.epm", (unsigned long long)meta_path_hash64(source_path));
    return std::string(app_paths::epub_metadata_dir()) + name;
}

bool EpubBook::cached_progress_info(const char *path, uint32_t spine, uint64_t position,
                                    int *percent, int *chapter, int *chapter_count)
{
    if (percent) *percent = 0;
    if (chapter) *chapter = 0;
    if (chapter_count) *chapter_count = 0;
    EpubBook cached;
    if (!cached.load_metadata_cache(path) || spine >= cached._spine.size()) return false;

    uint64_t total = 0;
    uint64_t before = 0;
    uint64_t current_size = 1;
    for (size_t i = 0; i < cached._spine.size(); ++i) {
        uint64_t size = std::max<uint64_t>(1, cached._spine[i].uncompressed_size);
        if (i < spine) before += size;
        if (i == spine) current_size = size;
        total += size;
    }
    if (total == 0) return false;

    // page_position is based on parsed content bytes while the metadata size
    // also includes markup. It is still a useful bounded estimate within the
    // current spine and is more accurate than spine index alone.
    uint64_t within = std::min<uint64_t>(position, current_size);
    uint64_t completed = std::min<uint64_t>(total, before + within);
    if (percent) {
        *percent = (int)std::min<uint64_t>(100, (completed * 100 + total / 2) / total);
    }

    if (!cached._toc.empty()) {
        int current_chapter = 0;
        for (size_t i = 0; i < cached._toc.size(); ++i) {
            if (cached._toc[i].spine_index <= (int)spine) current_chapter = (int)i + 1;
        }
        if (current_chapter == 0) current_chapter = 1;
        if (chapter) *chapter = current_chapter;
        if (chapter_count) *chapter_count = (int)cached._toc.size();
    }
    return true;
}

bool EpubBook::load_metadata_cache(const char *source_path)
{
    if (!source_path || !source_path[0]) return false;
    struct stat source_stat = {};
    if (stat(source_path, &source_stat) != 0) return false;

    std::string cache_path = metadata_cache_path(source_path);
    struct stat cache_stat = {};
    if (stat(cache_path.c_str(), &cache_stat) != 0 || cache_stat.st_size < 32 ||
        cache_stat.st_size > EPUB_META_MAX_BYTES) return false;

    FILE *fp = std::fopen(cache_path.c_str(), "rb");
    if (!fp) return false;
    std::vector<uint8_t> data((size_t)cache_stat.st_size);
    bool read_ok = std::fread(data.data(), 1, data.size(), fp) == data.size();
    std::fclose(fp);
    if (!read_ok) return false;

    MetaReader reader(data);
    uint32_t magic = 0, version = 0, spine_count_value = 0, toc_count_value = 0;
    uint64_t file_size = 0, mtime = 0;
    std::string saved_path;
    if (!reader.u32(&magic) || !reader.u32(&version) || !reader.u64(&file_size) ||
        !reader.u64(&mtime) || !reader.string(&saved_path) ||
        magic != EPUB_META_MAGIC || version != EPUB_META_VERSION ||
        file_size != (uint64_t)source_stat.st_size || mtime != (uint64_t)source_stat.st_mtime ||
        saved_path != source_path) return false;

    std::string cached_title, cached_opf_path, cached_opf_base;
    std::string cached_ncx_path, cached_nav_path, cached_cover_href;
    if (!reader.string(&cached_title) || !reader.string(&cached_opf_path) ||
        !reader.string(&cached_opf_base) || !reader.string(&cached_ncx_path) ||
        !reader.string(&cached_nav_path) || !reader.string(&cached_cover_href) ||
        !reader.u32(&spine_count_value) || spine_count_value == 0 || spine_count_value > 100000) return false;

    std::vector<EpubSpineItem> cached_spine;
    cached_spine.reserve(spine_count_value);
    for (uint32_t i = 0; i < spine_count_value; ++i) {
        EpubSpineItem item;
        if (!reader.string(&item.id) || !reader.string(&item.href) ||
            !reader.string(&item.media_type) || !reader.u32(&item.uncompressed_size) ||
            item.href.empty()) return false;
        cached_spine.push_back(std::move(item));
    }

    if (!reader.u32(&toc_count_value) || toc_count_value > 100000) return false;
    std::vector<EpubTocItem> cached_toc;
    cached_toc.reserve(toc_count_value);
    for (uint32_t i = 0; i < toc_count_value; ++i) {
        EpubTocItem item;
        uint32_t spine_index = 0, depth = 0;
        if (!reader.string(&item.title) || !reader.string(&item.href) ||
            !reader.u32(&spine_index) || !reader.u32(&depth) ||
            spine_index >= spine_count_value || depth > 1000) return false;
        item.spine_index = (int)spine_index;
        item.depth = (int)depth;
        cached_toc.push_back(std::move(item));
    }
    if (!reader.done()) return false;

    title = std::move(cached_title);
    _opf_path = std::move(cached_opf_path);
    _opf_base = std::move(cached_opf_base);
    _ncx_path = std::move(cached_ncx_path);
    _nav_path = std::move(cached_nav_path);
    _cover_image_href = std::move(cached_cover_href);
    _spine.swap(cached_spine);
    _toc.swap(cached_toc);
    rebuild_spine_index();
    return true;
}

void EpubBook::save_metadata_cache(const char *source_path) const
{
    if (!source_path || !source_path[0] || _spine.empty()) return;
    struct stat source_stat = {};
    if (stat(source_path, &source_stat) != 0) return;

    std::vector<uint8_t> data;
    data.reserve(128 + std::strlen(source_path) + _spine.size() * 64 + _toc.size() * 64);
    meta_append32(&data, EPUB_META_MAGIC);
    meta_append32(&data, EPUB_META_VERSION);
    meta_append64(&data, (uint64_t)source_stat.st_size);
    meta_append64(&data, (uint64_t)source_stat.st_mtime);
    if (!meta_append_string(&data, source_path) ||
        !meta_append_string(&data, title) || !meta_append_string(&data, _opf_path) ||
        !meta_append_string(&data, _opf_base) || !meta_append_string(&data, _ncx_path) ||
        !meta_append_string(&data, _nav_path) || !meta_append_string(&data, _cover_image_href)) return;

    meta_append32(&data, (uint32_t)_spine.size());
    for (const auto &item : _spine) {
        if (!meta_append_string(&data, item.id) || !meta_append_string(&data, item.href) ||
            !meta_append_string(&data, item.media_type)) return;
        meta_append32(&data, item.uncompressed_size);
    }

    meta_append32(&data, (uint32_t)_toc.size());
    for (const auto &item : _toc) {
        if (item.spine_index < 0 || item.spine_index >= (int)_spine.size() ||
            !meta_append_string(&data, item.title) || !meta_append_string(&data, item.href)) return;
        meta_append32(&data, (uint32_t)item.spine_index);
        meta_append32(&data, (uint32_t)std::max(0, item.depth));
    }
    if (data.size() > EPUB_META_MAX_BYTES) return;

    app_paths::ensure_dir(app_paths::epub_metadata_dir());
    std::string cache_path = metadata_cache_path(source_path);
    std::string temp_path = cache_path + ".tmp";
    FILE *fp = std::fopen(temp_path.c_str(), "wb");
    if (!fp) return;
    bool ok = std::fwrite(data.data(), 1, data.size(), fp) == data.size();
    std::fclose(fp);
    if (!ok) {
        std::remove(temp_path.c_str());
        return;
    }
    std::remove(cache_path.c_str());
    if (std::rename(temp_path.c_str(), cache_path.c_str()) != 0) std::remove(temp_path.c_str());
    else ESP_LOGI(TAG, "metadata cache saved bytes=%u path=%s", (unsigned)data.size(), cache_path.c_str());
}

bool EpubBook::read_item_to_string(const std::string &href, std::string *out)
{
    return _zip.read_entry_to_string(href, out, true);
}

bool EpubBook::read_item_prefix(const std::string &href, std::vector<uint8_t> *out,
                                size_t max_bytes)
{
    return _zip.read_entry_prefix(href, out, max_bytes);
}

bool EpubBook::read_item_to_memory(const std::string &href, std::vector<uint8_t> *out,
                                   uint32_t max_size)
{
    return _zip.read_entry_to_memory(href, out, max_size);
}

bool EpubBook::extract_item_to_file(const std::string &href, const std::string &out_path)
{
    return _zip.extract_entry_to_file(href, out_path);
}
