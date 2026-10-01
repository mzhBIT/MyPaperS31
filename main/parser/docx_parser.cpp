#include "parser/docx_parser.h"
#include "parser/epub_html.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_map>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr size_t XML_LIMIT = 8 * 1024 * 1024;
constexpr size_t HTML_LIMIT = 12 * 1024 * 1024;
constexpr size_t SECTION_TARGET = 64 * 1024;
struct Tag {
    std::string name;
    std::unordered_map<std::string, std::string> attrs;
    bool end = false, empty = false;
    size_t begin = 0, after = 0;
};
std::string local(const std::string &name)
{
    size_t colon = name.find(':');
    return name.substr(colon == std::string::npos ? 0 : colon + 1);
}
std::string attr(const Tag &tag, const char *key)
{
    auto it = tag.attrs.find(key);
    return it == tag.attrs.end() ? std::string() : it->second;
}
// No DTD/entity expansion, no DOM. Quotes and comments cannot terminate a tag.
bool next_tag(const std::string &xml, size_t *cursor, Tag *tag)
{
    while (*cursor < xml.size()) {
        const size_t start = xml.find('<', *cursor);
        if (start == std::string::npos) { *cursor = xml.size(); return false; }
        if (xml.compare(start, 4, "<!--") == 0) {
            const size_t end = xml.find("-->", start + 4);
            *cursor = end == std::string::npos ? xml.size() : end + 3;
            continue;
        }
        char quote = 0;
        size_t end = start + 1;
        for (; end < xml.size(); ++end) {
            const char c = xml[end];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '>') break;
        }
        if (end == xml.size()) { *cursor = end; return false; }
        *cursor = end + 1;
        if (xml[start + 1] == '?' || xml[start + 1] == '!') continue;
        *tag = {};
        tag->begin = start; tag->after = *cursor;
        size_t p = start + 1;
        tag->end = xml[p] == '/';
        if (tag->end) ++p;
        size_t first = p;
        while (p < end && !std::isspace((unsigned char)xml[p]) && xml[p] != '/') ++p;
        tag->name = local(xml.substr(first, p - first));
        tag->empty = xml[end - 1] == '/';
        while (p < end) {
            while (p < end && (std::isspace((unsigned char)xml[p]) || xml[p] == '/')) ++p;
            first = p;
            while (p < end && xml[p] != '=' && !std::isspace((unsigned char)xml[p])) ++p;
            std::string key = local(xml.substr(first, p - first));
            while (p < end && std::isspace((unsigned char)xml[p])) ++p;
            if (p >= end || xml[p++] != '=') break;
            while (p < end && std::isspace((unsigned char)xml[p])) ++p;
            if (p >= end || (xml[p] != '\'' && xml[p] != '"')) break;
            const char delimiter = xml[p++]; first = p;
            while (p < end && xml[p] != delimiter) ++p;
            tag->attrs[key] = xml.substr(first, p - first);
            if (p < end) ++p;
        }
        return !tag->name.empty();
    }
    return false;
}
void yield_due(int64_t *last)
{
    if (esp_timer_get_time() - *last >= 500000) {
        vTaskDelay(1);
        *last = esp_timer_get_time();
    }
}
bool read_xml(EpubZip &zip, const std::string &part, std::string *out, size_t limit)
{
    EpubZip::Entry info;
    return zip.entry_info(part, &info) && info.uncompressed_size <= limit &&
           zip.read_entry_to_string(part, out);
}
std::string text_of(const std::string &html)
{
    std::vector<EpubContentBlock> blocks;
    epub_parse_xhtml_blocks("<p>" + html + "</p>", "", &blocks);
    std::string text;
    for (const auto &block : blocks) if (block.type == EpubContentBlockType::Text) text += block.text;
    return text;
}
std::string escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}
std::string directory(const std::string &part)
{
    const size_t slash = part.find_last_of('/');
    return slash == std::string::npos ? "" : part.substr(0, slash + 1);
}
std::string resolve(const std::string &part, const std::string &raw)
{
    const std::string target = text_of(raw);
    if (target.empty() || target.find(':') != std::string::npos ||
        target.find('\\') != std::string::npos) return "";
    const std::string joined = target[0] == '/' ? target.substr(1) : directory(part) + target;
    std::vector<std::string> segments;
    for (size_t p = 0; p < joined.size();) {
        size_t end = joined.find('/', p);
        if (end == std::string::npos) end = joined.size();
        std::string segment = joined.substr(p, end - p); p = end + 1;
        if (segment == "..") { if (segments.empty()) return ""; segments.pop_back(); }
        else if (!segment.empty() && segment != ".") segments.push_back(segment);
    }
    std::string out;
    for (const auto &segment : segments) { if (!out.empty()) out += '/'; out += segment; }
    return out;
}
struct Relationship { std::string type, target; };
using Relationships = std::unordered_map<std::string, Relationship>;
Relationships relationships(EpubZip &zip, const std::string &part)
{
    std::string xml;
    std::string file;
    if (part.empty()) file = "_rels/.rels";
    else file = directory(part) + "_rels/" + part.substr(directory(part).size()) + ".rels";
    Relationships result;
    if (!read_xml(zip, file, &xml, 1024 * 1024)) return result;
    size_t pos = 0; Tag tag;
    while (next_tag(xml, &pos, &tag)) {
        if (tag.name != "Relationship" || tag.end || attr(tag, "TargetMode") == "External") continue;
        const std::string target = resolve(part, attr(tag, "Target"));
        if (!target.empty()) result[attr(tag, "Id")] = {attr(tag, "Type"), target};
    }
    return result;
}
std::string part_for(const Relationships &rels, const char *type)
{
    const std::string suffix = std::string("/") + type;
    for (const auto &entry : rels) {
        const std::string &value = entry.second.type;
        if (value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0)
            return entry.second.target;
    }
    return "";
}
bool enabled(const Tag &tag)
{
    const std::string value = attr(tag, "val");
    return value != "0" && value != "false" && value != "off";
}
struct Style { int heading = 0; int bold = -1, italic = -1; std::string parent; };
using Styles = std::unordered_map<std::string, Style>;
Styles read_styles(EpubZip &zip, const std::string &part)
{
    Styles styles; std::string xml;
    if (part.empty() || !read_xml(zip, part, &xml, 1024 * 1024)) return styles;
    size_t pos = 0; Tag tag; std::string id;
    while (next_tag(xml, &pos, &tag)) {
        if (tag.name == "style") { id = tag.end ? "" : attr(tag, "styleId"); continue; }
        if (id.empty() || tag.end) continue;
        auto &style = styles[id];
        if (tag.name == "outlineLvl") {
            const int value = std::atoi(attr(tag, "val").c_str());
            style.heading = value >= 0 && value < 6 ? value + 1 : 0;
        }
        if (tag.name == "basedOn") style.parent = attr(tag, "val");
        if (tag.name == "b") style.bold = enabled(tag);
        if (tag.name == "i") style.italic = enabled(tag);
        if (tag.name == "name") {
            const std::string name = attr(tag, "val");
            if (name.size() == 9 && name.compare(0, 8, "heading ") == 0 && name[8] >= '1' && name[8] <= '6')
                style.heading = name[8] - '0';
        }
    }
    return styles;
}
Style style_for(const Styles &styles, const std::string &id, int depth = 0)
{
    auto it = styles.find(id);
    if (depth > 8) return {};
    if (it == styles.end()) {
        Style fallback;
        std::string name = id;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (name.size() == 8 && name.compare(0, 7, "heading") == 0 && name[7] >= '1' && name[7] <= '6')
            fallback.heading = name[7] - '0';
        return fallback;
    }
    Style result = style_for(styles, it->second.parent, depth + 1);
    if (it->second.heading) result.heading = it->second.heading;
    if (it->second.bold >= 0) result.bold = it->second.bold;
    if (it->second.italic >= 0) result.italic = it->second.italic;
    return result;
}
struct Paragraph { std::string html, title; int heading = 0; };
// Word paragraphs/runs map to XHTML; rows and cells intentionally linearize.
bool paragraphs(const std::string &xml, const Relationships &rels, const Styles &styles,
                const std::string &notes_href, std::vector<Paragraph> *out)
{
    size_t pos = 0; Tag tag;
    bool in_p = false, in_text = false, in_run = false, numbered = false;
    int skip = 0, heading = 0, bold = -1, italic = -1;
    Style paragraph_style, run_style;
    std::string body;
    int64_t last_yield = esp_timer_get_time();
    size_t total = 0;
    std::vector<std::string> elements;
    auto flush = [&]() {
        if (body.empty()) return;
        if (numbered) body.insert(0, "&#8226; ");
        const std::string name = heading ? "h" + std::to_string(heading) : "p";
        std::string html = "<" + name + ">" + body + "</" + name + ">";
        out->push_back({std::move(html), heading ? text_of(body) : "", heading});
        total += out->back().html.size();
        body.clear();
    };
    auto styled_text = [&](const std::string &text) {
        const bool b = bold < 0 ? (run_style.bold < 0 ? paragraph_style.bold == 1 : run_style.bold == 1) : bold != 0;
        const bool i = italic < 0 ? (run_style.italic < 0 ? paragraph_style.italic == 1 : run_style.italic == 1) : italic != 0;
        if (b) body += "<strong>";
        if (i) body += "<em>";
        body += text;
        if (i) body += "</em>";
        if (b) body += "</strong>";
    };
    size_t previous = 0;
    while (next_tag(xml, &pos, &tag)) {
        yield_due(&last_yield);
        if (tag.end) {
            if (elements.empty() || elements.back() != tag.name) return false;
            elements.pop_back();
        } else if (!tag.empty) {
            if (elements.size() >= 256) return false;
            elements.push_back(tag.name);
        }
        if (in_text && !skip && tag.begin > previous) styled_text(xml.substr(previous, tag.begin - previous));
        previous = tag.after;
        if (tag.name == "del" || tag.name == "moveFrom" || tag.name == "Fallback") {
            if (tag.end) { if (skip) --skip; } else if (!tag.empty) ++skip;
            continue;
        }
        if (skip) continue;
        if (tag.name == "p") {
            if (tag.end) { flush(); in_p = false; }
            else {
                flush(); in_p = !tag.empty; in_text = false; in_run = false;
                numbered = false; heading = 0; paragraph_style = {}; body.clear();
            }
        } else if (in_p) {
            if (tag.name == "t") in_text = !tag.end && !tag.empty;
            else if (tag.name == "r") {
                in_run = !tag.end && !tag.empty; bold = italic = -1; run_style = {};
            } else if (!tag.end) {
                if (tag.name == "pStyle") {
                    paragraph_style = style_for(styles, attr(tag, "val")); heading = paragraph_style.heading;
                } else if (tag.name == "rStyle") run_style = style_for(styles, attr(tag, "val"));
                else if (tag.name == "outlineLvl") {
                    int level = std::atoi(attr(tag, "val").c_str());
                    heading = level >= 0 && level < 6 ? level + 1 : 0;
                } else if (tag.name == "b") { if (in_run) bold = enabled(tag); else paragraph_style.bold = enabled(tag); }
                else if (tag.name == "i") { if (in_run) italic = enabled(tag); else paragraph_style.italic = enabled(tag); }
                else if (tag.name == "numPr") numbered = true;
                else if (tag.name == "tab") body += " ";
                else if (tag.name == "br" || tag.name == "cr") body += "<br/>";
                else if (tag.name == "blip" || tag.name == "imagedata") {
                    auto image = rels.find(attr(tag, tag.name == "blip" ? "embed" : "id"));
                    if (image != rels.end()) {
                        // Absolute package href: existing HTML resolver strips leading slash.
                        body += "<img src=\"/" + escape(image->second.target) + "\"/>";
                    }
                } else if (tag.name == "footnoteReference" || tag.name == "endnoteReference") {
                    const std::string kind = tag.name == "footnoteReference" ? "fn" : "en";
                    const std::string id = text_of(attr(tag, "id"));
                    body += "<a epub:type=\"noteref\" href=\"/" + escape(notes_href) + "#" + kind + escape(id) + "\">[" + escape(id) + "]</a>";
                }
            }
        }
        if (body.size() + total > HTML_LIMIT) return false;
    }
    return !in_p && elements.empty() && !out->empty();
}
std::string notes(EpubZip &zip, const std::string &part, const char *element, const char *prefix,
                  const Styles &styles)
{
    std::string xml, result;
    if (part.empty() || !read_xml(zip, part, &xml, 2 * 1024 * 1024)) return result;
    const auto rels = relationships(zip, part);
    size_t pos = 0, start = 0; Tag tag; std::string id;
    while (next_tag(xml, &pos, &tag)) {
        if (tag.name != element) continue;
        if (!tag.end && !tag.empty) { id = text_of(attr(tag, "id")); start = tag.after; }
        else if (tag.end && !id.empty()) {
            if (id[0] != '-' && id != "0") {
                std::vector<Paragraph> content;
                paragraphs(xml.substr(start, tag.begin - start), rels, styles, "", &content);
                result += "<aside epub:type=\"footnote\" id=\"" + std::string(prefix) + escape(id) + "\">";
                for (const auto &p : content) result += p.html;
                result += "</aside>";
            }
            id.clear();
        }
    }
    return result;
}
} // namespace

bool DocxBook::open(const char *path)
{
    close();
    const int64_t began = esp_timer_get_time();
    if (!path || !zip_.open(path)) { error_ = "Could not open DOCX package"; return false; }
    const auto root = relationships(zip_, "");
    std::string part = part_for(root, "officeDocument");
    if (part.empty()) part = "word/document.xml";
    std::string xml;
    if (!read_xml(zip_, part, &xml, XML_LIMIT)) { error_ = "DOCX main document missing or too large"; return false; }
    size_t root_pos = 0; Tag document;
    if (!next_tag(xml, &root_pos, &document) || document.name != "document" || document.end) {
        error_ = "DOCX has no readable content or is malformed"; return false;
    }
    const auto rels = relationships(zip_, part);
    const auto styles = read_styles(zip_, part_for(rels, "styles"));
    const std::string base = directory(part);
    notes_href_ = base + "docx-notes.html";
    std::vector<Paragraph> content;
    if (!paragraphs(xml, rels, styles, notes_href_, &content)) {
        error_ = "DOCX has no readable content or is malformed"; return false;
    }
    std::string name(path);
    size_t slash = name.find_last_of("/\\"), dot = name.find_last_of('.');
    const size_t first = slash == std::string::npos ? 0 : slash + 1;
    title_ = name.substr(first, dot == std::string::npos || dot < first ? std::string::npos : dot - first);
    std::string metadata;
    const std::string core = part_for(root, "core-properties");
    if (!core.empty() && read_xml(zip_, core, &metadata, 64 * 1024)) {
        size_t pos = 0; Tag tag;
        while (next_tag(metadata, &pos, &tag)) if (tag.name == "title" && !tag.end && !tag.empty) {
            const size_t end = metadata.find('<', tag.after);
            const std::string value = text_of(metadata.substr(tag.after, end - tag.after));
            if (!value.empty()) title_ = value;
            break;
        }
    }
    const size_t xml_bytes = xml.size();
    std::string().swap(xml); // Do not retain main XML alongside all XHTML sections.
    for (auto &p : content) {
        if (sections_.empty() || (p.heading && !sections_.back().html.empty()) || sections_.back().html.size() >= SECTION_TARGET) {
            sections_.push_back({base + "docx-section-" + std::to_string(sections_.size()) + ".html",
                                 p.title.empty() ? title_ : p.title, "", std::max(0, p.heading - 1)});
        }
        if (sections_.back().html.empty()) sections_.back().html = std::move(p.html);
        else { sections_.back().html += p.html; std::string().swap(p.html); }
    }
    notes_html_ = notes(zip_, part_for(rels, "footnotes"), "footnote", "fn", styles) +
                  notes(zip_, part_for(rels, "endnotes"), "endnote", "en", styles);
    ESP_LOGI("DOCX", "opened sections=%u paragraphs=%u xml=%u time=%lld ms",
             (unsigned)sections_.size(), (unsigned)content.size(), (unsigned)xml_bytes,
             (long long)((esp_timer_get_time() - began) / 1000));
    return true;
}
void DocxBook::close()
{
    zip_.close(); title_.clear(); error_.clear();
    sections_.clear(); notes_html_.clear(); notes_href_.clear();
}
bool DocxBook::read_section(const std::string &href, std::string *out) const
{
    if (!out) return false;
    if (href == notes_href_) { *out = notes_html_; return !out->empty(); }
    for (const auto &section : sections_) if (section.href == href) { *out = section.html; return true; }
    return false;
}
bool DocxBook::read_prefix(const std::string &href, std::vector<uint8_t> *out, size_t size)
{ return zip_.read_entry_prefix(href, out, size); }
bool DocxBook::read_resource(const std::string &href, std::vector<uint8_t> *out, uint32_t size)
{ return zip_.read_entry_to_memory(href, out, size); }
bool DocxBook::extract_resource(const std::string &href, const std::string &path)
{ return zip_.extract_entry_to_file(href, path); }
