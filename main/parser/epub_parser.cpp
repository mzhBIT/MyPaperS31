#include "parser/epub_parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <esp_log.h>

static const char *TAG = "EPUB";

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
    close();
    if (!_zip.open(path)) return false;
    _path = path ? path : "";

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
    ESP_LOGI(TAG, "opened title=%s spine=%d toc=%d opf=%s", title.c_str(), spine_count(), toc_count(), _opf_path.c_str());
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
    for (size_t i = 0; i < _spine.size(); ++i) {
        if (_spine[i].href == target) return (int)i;
    }
    return -1;
}

bool EpubBook::read_item_to_string(const std::string &href, std::string *out)
{
    return _zip.read_entry_to_string(href, out, true);
}

bool EpubBook::extract_item_to_file(const std::string &href, const std::string &out_path)
{
    return _zip.extract_entry_to_file(href, out_path);
}
