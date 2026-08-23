#include "parser/epub_html.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

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
    return n > 0 ? (int)n : 0;
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

static void flush_content_block(std::string *current, std::vector<EpubContentBlock> *blocks,
                                int heading_level)
{
    std::string text = trim_copy(*current);
    current->clear();
    if (text.empty()) return;

    EpubContentBlock block;
    block.type = EpubContentBlockType::Text;
    block.text = text;
    block.heading = heading_level > 0;
    block.heading_level = heading_level;
    block.page_break_before = heading_level > 0 && heading_level <= 2 && !blocks->empty();
    block.blank_before = heading_level > 0 ? 1 : 0;
    block.blank_after = heading_level > 0 ? 1 : 0;
    blocks->push_back(block);
}

bool epub_parse_xhtml_blocks(const std::string &xhtml, const std::string &item_href, std::vector<EpubContentBlock> *blocks)
{
    if (!blocks) return false;
    blocks->clear();

    std::string current;
    std::string base = dirname_of(item_href);
    int heading_level = 0;
    int skip_depth = 0;
    size_t pos = 0;

    while (pos < xhtml.size()) {
        size_t tag_start = xhtml.find('<', pos);
        if (tag_start == std::string::npos) {
            if (skip_depth == 0) append_normalized_text(xhtml.substr(pos), &current);
            break;
        }

        if (tag_start > pos && skip_depth == 0) {
            append_normalized_text(xhtml.substr(pos, tag_start - pos), &current);
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

        if (!closing) {
            if (name == "pagebreak" || tag_requests_page_break(tag)) {
                flush_content_block(&current, blocks, heading_level);
                if (!blocks->empty() && blocks->back().type != EpubContentBlockType::PageBreak) {
                    EpubContentBlock page_break;
                    page_break.type = EpubContentBlockType::PageBreak;
                    blocks->push_back(page_break);
                }
            }
            if (name == "img") {
                flush_content_block(&current, blocks, heading_level);
                std::string parsed_name;
                std::unordered_map<std::string, std::string> attrs;
                if (parse_tag_attrs(tag, &parsed_name, &attrs)) {
                    std::string src = attr_value(attrs, "src");
                    if (src.empty()) src = attr_value(attrs, "href");
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
                flush_content_block(&current, blocks, heading_level);
                EpubContentBlock rule;
                rule.type = EpubContentBlockType::HorizontalRule;
                blocks->push_back(rule);
            } else if (is_block_tag(name)) {
                flush_content_block(&current, blocks, heading_level);
                int level = heading_level_for_tag(name);
                if (level > 0) heading_level = level;
            }
        } else {
            if (is_block_tag(name)) {
                flush_content_block(&current, blocks, heading_level);
                if (is_heading_tag(name)) heading_level = 0;
            }
        }

        pos = tag_end + 1;
    }

    flush_content_block(&current, blocks, heading_level);
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
