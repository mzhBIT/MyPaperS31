#include "parser/epub_html.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

static std::string lower_name(std::string name)
{
    size_t colon = name.find(':');
    if (colon != std::string::npos) name.erase(0, colon + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return name;
}

static std::string trim_copy(const std::string &s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string dirname_of(const std::string &path)
{
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string() : path.substr(0, p + 1);
}

static std::string normalize_join(const std::string &base, const std::string &href)
{
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) return href;
    if (href.rfind("//", 0) == 0) return "https:" + href;
    std::string path = href;
    size_t hash = path.find('#');
    if (hash != std::string::npos) path.erase(hash);
    size_t query = path.find('?');
    if (query != std::string::npos) path.erase(query);
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

static void append_utf8(uint32_t cp, std::string *out)
{
    if (cp < 0x80) {
        out->push_back((char)cp);
    } else if (cp < 0x800) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out->push_back((char)(0xF0 | (cp >> 18)));
        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static std::string decode_entities(const std::string &s)
{
    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }

        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 16) {
            out.push_back(s[i++]);
            continue;
        }

        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent == "nbsp") out.push_back(' ');
        else if (!ent.empty() && ent[0] == '#') {
            char *endp = nullptr;
            uint32_t cp = 0;
            if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                cp = (uint32_t)std::strtoul(ent.c_str() + 2, &endp, 16);
            } else {
                cp = (uint32_t)std::strtoul(ent.c_str() + 1, &endp, 10);
            }
            if (endp && *endp == '\0' && cp > 0 && cp <= 0x10FFFF) {
                append_utf8(cp, &out);
            }
        } else {
            out.append(s, i, semi - i + 1);
        }
        i = semi + 1;
    }

    return out;
}

static bool is_block_tag(const std::string &name)
{
    return name == "p" || name == "div" || name == "section" || name == "article" ||
           name == "body" || name == "blockquote" || name == "li" ||
           name == "h1" || name == "h2" || name == "h3" || name == "h4" ||
           name == "h5" || name == "h6";
}

static bool is_heading_tag(const std::string &name)
{
    return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

static int heading_level_for_tag(const std::string &name)
{
    return is_heading_tag(name) ? name[1] - '0' : 0;
}

static std::string tag_name_from(const std::string &tag)
{
    size_t pos = 0;
    while (pos < tag.size() && std::isspace((unsigned char)tag[pos])) pos++;
    if (pos < tag.size() && tag[pos] == '/') pos++;
    while (pos < tag.size() && std::isspace((unsigned char)tag[pos])) pos++;

    size_t start = pos;
    while (pos < tag.size() && !std::isspace((unsigned char)tag[pos]) && tag[pos] != '/' && tag[pos] != '>') pos++;
    return lower_name(tag.substr(start, pos - start));
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
    *name = lower_name(tag.substr(start, pos - start));

    while (pos < tag.size()) {
        while (pos < tag.size() && (std::isspace((unsigned char)tag[pos]) || tag[pos] == '/')) pos++;
        if (pos >= tag.size()) break;

        size_t key_start = pos;
        while (pos < tag.size() && tag[pos] != '=' && !std::isspace((unsigned char)tag[pos]) && tag[pos] != '/') pos++;
        std::string key = lower_name(tag.substr(key_start, pos - key_start));
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
        (*attrs)[key] = decode_entities(value);
    }

    return true;
}

static std::string attr_value(const std::unordered_map<std::string, std::string> &attrs, const char *key)
{
    auto it = attrs.find(key);
    return it == attrs.end() ? std::string() : it->second;
}

static bool tag_requests_page_break(const std::string &tag)
{
    std::string name;
    std::unordered_map<std::string, std::string> attrs;
    if (!parse_tag_attrs(tag, &name, &attrs)) return false;
    std::string style = attr_value(attrs, "style");
    std::transform(style.begin(), style.end(), style.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    bool legacy = style.find("page-break-before") != std::string::npos &&
                  style.find("always") != std::string::npos;
    bool modern = style.find("break-before") != std::string::npos &&
                  style.find("page") != std::string::npos;
    return legacy || modern;
}

static int parse_positive_px(const std::string &value)
{
    if (value.empty() || value.find('%') != std::string::npos) return 0;
    char *endp = nullptr;
    long n = std::strtol(value.c_str(), &endp, 10);
    if (n <= 0 || endp == value.c_str()) return 0;
    while (*endp && std::isspace(static_cast<unsigned char>(*endp))) ++endp;
    if (*endp && lower_name(endp) != "px") return 0;
    return static_cast<int>(n);
}

static void append_normalized_text(const std::string &raw, std::string *current)
{
    std::string decoded = decode_entities(raw);
    bool last_space = current->empty() || current->back() == ' ' || current->back() == '\n';

    for (size_t i = 0; i < decoded.size(); ++i) {
        unsigned char c = (unsigned char)decoded[i];
        if (std::isspace(c)) {
            if (!last_space) {
                current->push_back(' ');
                last_space = true;
            }
        } else {
            current->push_back((char)c);
            last_space = false;
        }
    }
}

static std::string normalized_link_target(const std::string &item_href, const std::string &href)
{
    const size_t hash = href.find('#');
    if (hash == std::string::npos || hash + 1 >= href.size()) return std::string();
    std::string file = href.substr(0, hash);
    const std::string anchor = href.substr(hash + 1);
    if (file.empty()) file = item_href;
    else file = normalize_join(dirname_of(item_href), file);
    return file + "#" + anchor;
}

static std::string plain_text_from_html(const std::string &html)
{
    std::string text;
    size_t pos = 0;
    while (pos < html.size()) {
        const size_t tag = html.find('<', pos);
        if (tag == std::string::npos) {
            append_normalized_text(html.substr(pos), &text);
            break;
        }
        if (tag > pos) append_normalized_text(html.substr(pos, tag - pos), &text);
        const size_t end = html.find('>', tag + 1);
        if (end == std::string::npos) break;
        const std::string name = tag_name_from(html.substr(tag + 1, end - tag - 1));
        if (name == "br") append_normalized_text(" ", &text);
        pos = end + 1;
    }
    return trim_copy(text);
}

static void remember_footnote_definition(
    std::unordered_map<std::string, std::string> *definitions,
    const std::string &item_href, const std::string &id,
    const std::string &html)
{
    if (!definitions || id.empty()) return;
    std::string content = plain_text_from_html(html);
    if (content.empty() || content.size() > 6000) return;
    const std::string key = item_href + "#" + id;
    const auto existing = definitions->find(key);
    if (existing == definitions->end() || content.size() < existing->second.size()) {
        (*definitions)[key] = std::move(content);
    }
}

static std::unordered_map<std::string, std::string> collect_footnote_definitions(
    const std::string &xhtml, const std::string &item_href)
{
    std::unordered_map<std::string, std::string> definitions;
    std::string lower = xhtml;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    size_t pos = 0;
    while ((pos = xhtml.find('<', pos)) != std::string::npos) {
        const size_t tag_end = xhtml.find('>', pos + 1);
        if (tag_end == std::string::npos) break;
        const std::string raw = xhtml.substr(pos + 1, tag_end - pos - 1);
        const bool closing = !raw.empty() && raw[0] == '/';
        const bool self_closing = !raw.empty() && raw.back() == '/';
        std::string name;
        std::unordered_map<std::string, std::string> attrs;
        if (!closing && !self_closing && parse_tag_attrs(raw, &name, &attrs)) {
            const bool definition_tag = name == "p" || name == "li" || name == "aside" ||
                                        name == "dd" || name == "td" || name == "div";
            std::string id = attr_value(attrs, "id");
            if (id.empty()) id = attr_value(attrs, "name");
            if (definition_tag) {
                const std::string close = "</" + name;
                const size_t close_start = lower.find(close, tag_end + 1);
                if (close_start != std::string::npos) {
                    const std::string inner = xhtml.substr(tag_end + 1, close_start - tag_end - 1);
                    // Avoid flattening/scanning a chapter-sized root <div>.
                    // Genuine footnotes are small; 12 KB leaves ample markup
                    // overhead for the 6 KB cleaned-text limit below.
                    if (inner.size() > 12000) {
                        pos = tag_end + 1;
                        continue;
                    }
                    remember_footnote_definition(&definitions, item_href, id, inner);

                    // A number of local EPUB generators put the target id on
                    // a child <a>/<span> instead of the enclosing note block.
                    size_t inner_pos = 0;
                    while ((inner_pos = inner.find('<', inner_pos)) != std::string::npos) {
                        const size_t inner_end = inner.find('>', inner_pos + 1);
                        if (inner_end == std::string::npos) break;
                        std::string child_name;
                        std::unordered_map<std::string, std::string> child_attrs;
                        const std::string child = inner.substr(inner_pos + 1, inner_end - inner_pos - 1);
                        if (!child.empty() && child[0] != '/' &&
                            parse_tag_attrs(child, &child_name, &child_attrs)) {
                            std::string child_id = attr_value(child_attrs, "id");
                            if (child_id.empty()) child_id = attr_value(child_attrs, "name");
                            remember_footnote_definition(&definitions, item_href, child_id, inner);
                        }
                        inner_pos = inner_end + 1;
                    }
                }
            } else if (name == "a" && !id.empty()) {
                // Some EPUB generators put an empty named anchor at the
                // beginning of the following note text inside a parent <p> or
                // <li>. Capture text up to the nearest enclosing block close.
                const size_t anchor_close = lower.find("</a", tag_end + 1);
                size_t anchor_end = tag_end + 1;
                if (anchor_close != std::string::npos) {
                    const size_t close_end = xhtml.find('>', anchor_close + 3);
                    if (close_end != std::string::npos) anchor_end = close_end + 1;
                }
                size_t nearest = std::string::npos;
                for (const char *closing_tag : {"</p", "</li", "</aside", "</dd", "</td"}) {
                    const size_t candidate = lower.find(closing_tag, anchor_end);
                    if (candidate != std::string::npos && (nearest == std::string::npos || candidate < nearest)) {
                        nearest = candidate;
                    }
                }
                if (nearest != std::string::npos && nearest > anchor_end && nearest - anchor_end <= 12000) {
                    std::string content = plain_text_from_html(
                        xhtml.substr(anchor_end, nearest - anchor_end));
                    if (!content.empty() && content.size() <= 6000) {
                        definitions[item_href + "#" + id] = std::move(content);
                    }
                }
            }
        }
        pos = tag_end + 1;
    }
    return definitions;
}

static bool contains_footnote_token(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "note" || // FictionBook <a type="note">.
           value.find("noteref") != std::string::npos ||
           value.find("footnote") != std::string::npos ||
           value.find("fn-ref") != std::string::npos ||
           value.find("note-ref") != std::string::npos;
}

static bool is_weread_annotation(const std::unordered_map<std::string, std::string> &attrs)
{
    std::string class_name = attr_value(attrs, "class");
    std::transform(class_name.begin(), class_name.end(), class_name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return class_name.find("wr-annotation") != std::string::npos &&
           !attr_value(attrs, "data-wr-range").empty();
}

static bool looks_like_footnote_marker(std::string text)
{
    text = trim_copy(text);
    if (text.empty() || text.size() > 24) return false;
    if (text == "*" || text == "**" || text == "†" || text == "‡") return true;
    size_t begin = 0, end = text.size();
    if ((text.front() == '[' && text.back() == ']') ||
        (text.front() == '(' && text.back() == ')')) {
        begin = 1;
        --end;
    }
    if (begin >= end) return false;
    for (size_t i = begin; i < end; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

static bool is_footnote_backlink(const std::unordered_map<std::string, std::string> &attrs,
                                 const std::string &target)
{
    std::string semantic = attr_value(attrs, "class") + " " +
                           attr_value(attrs, "role") + " " +
                           attr_value(attrs, "type");
    std::transform(semantic.begin(), semantic.end(), semantic.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::string lower_target = target;
    std::transform(lower_target.begin(), lower_target.end(), lower_target.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return semantic.find("backlink") != std::string::npos ||
           semantic.find("backref") != std::string::npos ||
           semantic.find("doc-backlink") != std::string::npos ||
           semantic.find("footnote-back") != std::string::npos ||
           semantic.find("note-back") != std::string::npos ||
           lower_target.find("#wrfnref-") != std::string::npos;
}

static bool image_html_looks_like_footnote(const std::string &inner)
{
    std::string lower = inner;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("qqreader-footnote") != std::string::npos ||
           lower.find("footnote-icon") != std::string::npos ||
           lower.find("footnote-ref") != std::string::npos ||
           lower.find("note-ref") != std::string::npos ||
           lower.find("note.png") != std::string::npos;
}

static std::unordered_set<std::string> collect_footnote_reference_targets(
    const std::string &xhtml, const std::string &item_href)
{
    std::unordered_set<std::string> targets;
    std::string lower = xhtml;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    size_t pos = 0;
    while ((pos = lower.find("<a", pos)) != std::string::npos) {
        if (pos + 2 < lower.size() && !std::isspace(static_cast<unsigned char>(lower[pos + 2])) &&
            lower[pos + 2] != '>') {
            pos += 2;
            continue;
        }
        const size_t open_end = xhtml.find('>', pos + 2);
        if (open_end == std::string::npos) break;
        const size_t close_start = lower.find("</a", open_end + 1);
        if (close_start == std::string::npos) break;
        const size_t close_end = xhtml.find('>', close_start + 3);
        if (close_end == std::string::npos) break;

        std::string name;
        std::unordered_map<std::string, std::string> attrs;
        const std::string tag = xhtml.substr(pos + 1, open_end - pos - 1);
        if (parse_tag_attrs(tag, &name, &attrs)) {
            const std::string href = attr_value(attrs, "href");
            const std::string target = normalized_link_target(item_href, href);
            const std::string inner = xhtml.substr(open_end + 1, close_start - open_end - 1);
            const bool semantic = contains_footnote_token(attr_value(attrs, "class")) ||
                                  contains_footnote_token(attr_value(attrs, "role")) ||
                                  contains_footnote_token(attr_value(attrs, "type"));
            std::string lower_href = href;
            std::transform(lower_href.begin(), lower_href.end(), lower_href.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            const size_t hash = lower_href.find('#');
            const std::string file = hash == std::string::npos ? lower_href : lower_href.substr(0, hash);
            const std::string anchor = hash == std::string::npos ? std::string() : lower_href.substr(hash + 1);
            const bool marker_hint = file.empty() ||
                                     file.find("note") != std::string::npos ||
                                     file.find(".xhtml") != std::string::npos ||
                                     file.find(".html") != std::string::npos ||
                                     anchor.find("note") != std::string::npos ||
                                     anchor.find("foot") != std::string::npos ||
                                     anchor.rfind("fn", 0) == 0 ||
                                     anchor.rfind("ref", 0) == 0;
            if (!target.empty() && !is_footnote_backlink(attrs, target) &&
                (semantic || (looks_like_footnote_marker(plain_text_from_html(inner)) && marker_hint) ||
                 image_html_looks_like_footnote(inner))) {
                targets.insert(target);
            }
        }
        pos = close_end + 1;
    }
    return targets;
}

static void flush_content_block(std::string *current, std::vector<EpubContentBlock> *blocks,
                                int heading_level, std::vector<EpubFootnoteRef> *footnotes,
                                int64_t *source_delta,
                                std::vector<EpubSourcePoint> *source_points,
                                std::vector<inline_style::Span> *styles)
{
    size_t leading = 0;
    while (leading < current->size() && std::isspace(static_cast<unsigned char>((*current)[leading]))) ++leading;
    size_t trailing = current->size();
    while (trailing > leading && std::isspace(static_cast<unsigned char>((*current)[trailing - 1]))) --trailing;
    std::string text = current->substr(leading, trailing - leading);
    current->clear();
    const int64_t delta = source_delta ? *source_delta : 0;
    if (source_delta) *source_delta = 0;
    if (text.empty()) {
        if (styles) styles->clear();
        if (footnotes) footnotes->clear();
        if (source_points) source_points->clear();
        return;
    }

    EpubContentBlock block;
    block.type = EpubContentBlockType::Text;
    block.text = text;
    block.source_text_bytes = static_cast<size_t>(std::max<int64_t>(0,
        static_cast<int64_t>(text.size()) + delta));
    if (source_points && !source_points->empty()) {
        block.source_points.push_back({0, 0});
        for (const auto &point : *source_points) {
            if (point.display <= leading || point.display >= trailing) continue;
            block.source_points.push_back({point.display - leading,
                point.source > leading ? point.source - leading : 0});
        }
        block.source_points.push_back({text.size(), block.source_text_bytes});
        source_points->clear();
    }
    block.heading = heading_level > 0;
    block.heading_level = heading_level;
    block.page_break_before = heading_level > 0 && heading_level <= 2 && !blocks->empty();
    block.blank_before = heading_level > 0 ? 1 : 0;
    block.blank_after = heading_level > 0 ? 1 : 0;
    if (footnotes) {
        for (auto &ref : *footnotes) {
            if (ref.byte_end <= leading || ref.byte_start >= trailing) continue;
            ref.byte_start = ref.byte_start > leading ? ref.byte_start - leading : 0;
            ref.byte_end = std::min(ref.byte_end, trailing) - leading;
            if (ref.byte_start < ref.byte_end) block.footnotes.push_back(std::move(ref));
        }
        footnotes->clear();
    }
    if (styles) {
        for (auto span : *styles) {
            if (span.byte_end <= leading || span.byte_start >= trailing) continue;
            span.byte_start = span.byte_start > leading ? span.byte_start - leading : 0;
            span.byte_end = std::min(span.byte_end, trailing) - leading;
            if (span.byte_start < span.byte_end) block.styles.push_back(span);
        }
        styles->clear();
    }
    blocks->push_back(block);
}

bool epub_parse_xhtml_blocks(const std::string &xhtml, const std::string &item_href,
                             std::vector<EpubContentBlock> *blocks,
                             bool inline_sup_annotations, bool preserve_inline_styles)
{
    if (!blocks) return false;
    blocks->clear();

    std::string current;
    std::string base = dirname_of(item_href);
    const auto footnote_definitions = collect_footnote_definitions(xhtml, item_href);
    const auto footnote_targets = collect_footnote_reference_targets(xhtml, item_href);
    std::vector<EpubFootnoteRef> pending_footnotes;
    int64_t source_delta = 0;
    std::vector<EpubSourcePoint> pending_source_points;
    std::vector<inline_style::Span> pending_styles;
    int bold_depth = 0, italic_depth = 0;
    auto append_text = [&](const std::string &raw) {
        const size_t start = current.size();
        append_normalized_text(raw, &current);
        const uint8_t flags = (bold_depth ? inline_style::Bold : 0) |
                              (italic_depth ? inline_style::Italic : 0);
        if (preserve_inline_styles && flags && current.size() > start) {
            if (!pending_styles.empty() && pending_styles.back().flags == flags &&
                pending_styles.back().byte_end == start) pending_styles.back().byte_end = current.size();
            else pending_styles.push_back({start, current.size(), flags});
        }
    };
    struct ActiveLink {
        bool active = false;
        bool semantic = false;
        size_t byte_start = 0;
        std::string target;
        std::string content;
    } active_link;
    struct ActiveAnnotation {
        bool active = false;
        size_t byte_start = 0;
        std::string range;
    } active_annotation;
    int heading_level = 0;
    int skip_depth = 0;
    size_t pos = 0;

    while (pos < xhtml.size()) {
        size_t tag_start = xhtml.find('<', pos);
        if (tag_start == std::string::npos) {
            if (skip_depth == 0) append_text(xhtml.substr(pos));
            break;
        }

        if (tag_start > pos && skip_depth == 0) {
            append_text(xhtml.substr(pos, tag_start - pos));
        }

        if (xhtml.compare(tag_start, 4, "<!--") == 0) {
            size_t comment_end = xhtml.find("-->", tag_start + 4);
            pos = comment_end == std::string::npos ? xhtml.size() : comment_end + 3;
            continue;
        }

        size_t tag_end = xhtml.find('>', tag_start + 1);
        if (tag_end == std::string::npos) break;
        std::string tag = xhtml.substr(tag_start + 1, tag_end - tag_start - 1);
        bool closing = !tag.empty() && tag[0] == '/';
        bool self_closing = !tag.empty() && tag[tag.size() - 1] == '/';
        std::string name = tag_name_from(tag);
        std::string parsed_name;
        std::unordered_map<std::string, std::string> attrs;
        const bool parsed_attrs = !closing && parse_tag_attrs(tag, &parsed_name, &attrs);

        if (name == "script" || name == "style" || name == "head" || name == "svg") {
            if (!closing && !self_closing) skip_depth++;
            else if (closing && skip_depth > 0) skip_depth--;
            pos = tag_end + 1;
            continue;
        }

        if (skip_depth > 0) {
            pos = tag_end + 1;
            continue;
        }
        if (preserve_inline_styles && !self_closing) {
            if (name == "strong" || name == "b") bold_depth = std::max(0, bold_depth + (closing ? -1 : 1));
            if (name == "em" || name == "i") italic_depth = std::max(0, italic_depth + (closing ? -1 : 1));
        }

        // Ruby text is an annotation of the base glyph, not another sentence
        // following it. Keep the base in the normal text stream and attach the
        // reading to its exact UTF-8 byte range for the existing note popup.
        if (name == "ruby" && !closing) {
            const size_t rt_open = xhtml.find("<rt", tag_end + 1);
            const size_t ruby_close = xhtml.find("</ruby", tag_end + 1);
            if (rt_open != std::string::npos && ruby_close != std::string::npos && rt_open < ruby_close) {
                const size_t rt_tag_end = xhtml.find('>', rt_open + 3);
                const size_t rt_close = rt_tag_end == std::string::npos
                    ? std::string::npos : xhtml.find("</rt", rt_tag_end + 1);
                const size_t rt_end = rt_close == std::string::npos
                    ? std::string::npos : xhtml.find('>', rt_close + 4);
                const size_t ruby_end = xhtml.find('>', ruby_close + 6);
                if (rt_end != std::string::npos && rt_end < ruby_close && ruby_end != std::string::npos) {
                    std::string base_text = plain_text_from_html(xhtml.substr(tag_end + 1, rt_open - tag_end - 1));
                    base_text += plain_text_from_html(xhtml.substr(rt_end + 1, ruby_close - rt_end - 1));
                    const std::string reading = plain_text_from_html(
                        xhtml.substr(rt_tag_end + 1, rt_close - rt_tag_end - 1));
                    const size_t anchor = current.size();
                    append_normalized_text(base_text, &current);
                    source_delta += static_cast<int64_t>(reading.size());
                    pending_source_points.push_back({current.size(), static_cast<size_t>(
                        std::max<int64_t>(0, static_cast<int64_t>(current.size()) + source_delta))});
                    if (!reading.empty() && anchor < current.size()) {
                        EpubFootnoteRef ref;
                        ref.byte_start = anchor;
                        ref.byte_end = current.size();
                        ref.content = reading;
                        ref.kind = EpubFootnoteRef::Kind::RubyText;
                        pending_footnotes.push_back(std::move(ref));
                    }
                    pos = ruby_end + 1;
                    continue;
                }
            }
        }

        // In this vertical KF8 book, <sup> is visible small inline commentary,
        // not a link to a separate footnote. Preserve it in reading order and
        // let the layout/renderer use the book's 65% superscript size.
        if (inline_sup_annotations && name == "sup" && !closing) {
            const size_t sup_close = xhtml.find("</sup", tag_end + 1);
            if (sup_close != std::string::npos && sup_close - tag_end <= 16384) {
                const size_t sup_end = xhtml.find('>', sup_close + 5);
                if (sup_end != std::string::npos) {
                    const std::string note = plain_text_from_html(
                        xhtml.substr(tag_end + 1, sup_close - tag_end - 1));
                    if (!note.empty()) {
                        EpubFootnoteRef ref;
                        ref.byte_start = current.size();
                        append_normalized_text(note, &current);
                        ref.byte_end = current.size();
                        ref.kind = EpubFootnoteRef::Kind::InlineSup;
                        if (ref.byte_start < ref.byte_end) pending_footnotes.push_back(std::move(ref));
                    }
                    pos = sup_end + 1;
                    continue;
                }
            }
        }

        if (name == "a") {
            if (!closing && parsed_attrs) {
                const std::string href = attr_value(attrs, "href");
                const std::string target = normalized_link_target(item_href, href);
                if (!target.empty()) {
                    active_link.active = true;
                    active_link.byte_start = current.size();
                    active_link.target = target;
                    active_link.semantic = contains_footnote_token(attr_value(attrs, "class")) ||
                                           contains_footnote_token(attr_value(attrs, "role")) ||
                                           contains_footnote_token(attr_value(attrs, "type"));
                    if (is_footnote_backlink(attrs, target)) active_link.active = false;
                }
            } else if (closing && active_link.active) {
                const size_t byte_end = current.size();
                const std::string marker = byte_end > active_link.byte_start
                    ? current.substr(active_link.byte_start, byte_end - active_link.byte_start)
                    : std::string();
                const auto definition = footnote_definitions.find(active_link.target);
                if (active_link.semantic || footnote_targets.count(active_link.target)) {
                    EpubFootnoteRef ref;
                    ref.byte_start = active_link.byte_start;
                    ref.byte_end = byte_end;
                    ref.target = active_link.target;
                    ref.content = active_link.content;
                    if (ref.content.empty() && definition != footnote_definitions.end()) {
                        ref.content = definition->second;
                    }
                    if (ref.byte_start < ref.byte_end) pending_footnotes.push_back(std::move(ref));
                }
                active_link = ActiveLink();
            }
        }

        if (name == "span") {
            if (!closing && parsed_attrs && is_weread_annotation(attrs)) {
                active_annotation.active = true;
                active_annotation.byte_start = current.size();
                active_annotation.range = attr_value(attrs, "data-wr-range");
            } else if (closing && active_annotation.active) {
                EpubFootnoteRef ref;
                ref.byte_start = active_annotation.byte_start;
                ref.byte_end = current.size();
                ref.target = item_href + "#wrannotation-" + active_annotation.range;
                ref.kind = EpubFootnoteRef::Kind::WereadComment;
                if (ref.byte_start < ref.byte_end) pending_footnotes.push_back(std::move(ref));
                active_annotation = ActiveAnnotation();
            }
        }

        if (!closing) {
            if (name == "pagebreak" || tag_requests_page_break(tag)) {
                flush_content_block(&current, blocks, heading_level, &pending_footnotes,
                                    &source_delta, &pending_source_points, &pending_styles);
                if (!blocks->empty() && blocks->back().type != EpubContentBlockType::PageBreak) {
                    EpubContentBlock page_break;
                    page_break.type = EpubContentBlockType::PageBreak;
                    blocks->push_back(page_break);
                }
            }
            if (name == "img") {
                if (parsed_attrs) {
                    std::string src = attr_value(attrs, "src");
                    if (src.empty()) src = attr_value(attrs, "href");
                    const std::string class_name = attr_value(attrs, "class");
                    std::string note_content = attr_value(attrs, "alt");
                    if (note_content.empty()) note_content = attr_value(attrs, "title");
                    if (note_content.empty()) note_content = attr_value(attrs, "data-content");
                    if (note_content.empty()) note_content = attr_value(attrs, "data-note");
                    const bool footnote_image = contains_footnote_token(class_name);
                    const bool linked_marker = active_link.active &&
                        lower_name(src.substr(src.find_last_of("/\\") + 1)) == "note.png";
                    if (footnote_image || linked_marker) {
                        const size_t marker_start = current.size();
                        append_normalized_text("[*]", &current);
                        if (active_link.active) {
                            if (!note_content.empty()) active_link.content = trim_copy(note_content);
                        } else if (!note_content.empty()) {
                            EpubFootnoteRef ref;
                            ref.byte_start = marker_start;
                            ref.byte_end = current.size();
                            ref.content = trim_copy(note_content);
                            pending_footnotes.push_back(std::move(ref));
                        }
                        pos = tag_end + 1;
                        continue;
                    }
                    flush_content_block(&current, blocks, heading_level, &pending_footnotes,
                                        &source_delta, &pending_source_points, &pending_styles);
                    if (!src.empty()) {
                        EpubContentBlock img;
                        img.type = EpubContentBlockType::Image;
                        img.href = normalize_join(base, src);
                        img.image_width = parse_positive_px(attr_value(attrs, "width"));
                        img.image_height = parse_positive_px(attr_value(attrs, "height"));
                        img.blank_before = 1;
                        img.blank_after = 1;
                        blocks->push_back(img);
                    }
                }
            } else if (name == "br") {
                if (!current.empty() && current.back() != '\n') current.push_back('\n');
            } else if (name == "hr") {
                flush_content_block(&current, blocks, heading_level, &pending_footnotes,
                                    &source_delta, &pending_source_points, &pending_styles);
                EpubContentBlock rule;
                rule.type = EpubContentBlockType::HorizontalRule;
                blocks->push_back(rule);
            } else if (is_block_tag(name)) {
                flush_content_block(&current, blocks, heading_level, &pending_footnotes,
                                    &source_delta, &pending_source_points, &pending_styles);
                int level = heading_level_for_tag(name);
                if (level > 0) heading_level = level;
            }
        } else {
            if (is_block_tag(name)) {
                flush_content_block(&current, blocks, heading_level, &pending_footnotes,
                                    &source_delta, &pending_source_points, &pending_styles);
                if (is_heading_tag(name)) heading_level = 0;
            }
        }

        pos = tag_end + 1;
    }

    flush_content_block(&current, blocks, heading_level, &pending_footnotes,
                        &source_delta, &pending_source_points, &pending_styles);

    // Popup footnotes must not consume normal pagination space. Limit removal
    // to definition text reached by a reference classified above; arbitrary
    // id-bearing paragraphs and ordinary internal links remain untouched.
    std::unordered_set<std::string> hidden_definition_text;
    for (const auto &target : footnote_targets) {
        const auto definition = footnote_definitions.find(target);
        if (definition != footnote_definitions.end()) {
            hidden_definition_text.insert(trim_copy(definition->second));
        }
    }
    if (!hidden_definition_text.empty()) {
        blocks->erase(std::remove_if(blocks->begin(), blocks->end(), [&](const EpubContentBlock &block) {
            return block.type == EpubContentBlockType::Text &&
                   hidden_definition_text.count(trim_copy(block.text)) != 0;
        }), blocks->end());
    }
    return true;
}

bool epub_parse_xhtml_text_blocks(const std::string &xhtml, std::vector<EpubTextBlock> *blocks)
{
    if (!blocks) return false;
    std::vector<EpubContentBlock> content;
    if (!epub_parse_xhtml_blocks(xhtml, std::string(), &content)) return false;

    blocks->clear();
    for (const auto &item : content) {
        if (item.type != EpubContentBlockType::Text) continue;
        EpubTextBlock block;
        block.text = item.text;
        block.blank_before = item.blank_before;
        block.blank_after = item.blank_after;
        block.heading = item.heading;
        block.heading_level = item.heading_level;
        block.page_break_before = item.page_break_before;
        blocks->push_back(block);
    }
    return true;
}

bool epub_resolve_footnote(const std::string &xhtml, const std::string &item_href,
                           const std::string &target, std::string *content)
{
    if (!content || target.empty()) return false;
    const auto definitions = collect_footnote_definitions(xhtml, item_href);
    const auto found = definitions.find(target);
    if (found == definitions.end() || found->second.empty()) return false;
    *content = found->second;
    return true;
}
