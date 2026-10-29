#include "parser/fb2_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/stat.h>

namespace {

constexpr const char *TAG = "FB2";
constexpr uint64_t MAX_FB2_BYTES = 128ULL * 1024ULL * 1024ULL;

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    });
    return value;
}

size_t find_ascii_ci(const std::string &text, const std::string &needle, size_t from = 0)
{
    if (needle.empty()) return std::min(from, text.size());
    if (needle.size() > text.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= text.size(); ++i) {
        size_t j = 0;
        while (j < needle.size()) {
            const unsigned char a = static_cast<unsigned char>(text[i + j]);
            const unsigned char b = static_cast<unsigned char>(needle[j]);
            const unsigned char folded = a < 0x80 ? static_cast<unsigned char>(std::tolower(a)) : a;
            if (folded != b) break;
            ++j;
        }
        if (j == needle.size()) return i;
    }
    return std::string::npos;
}

std::string local_name(std::string value)
{
    const size_t colon = value.find(':');
    if (colon != std::string::npos) value.erase(0, colon + 1);
    return lower_ascii(value);
}

std::string tag_name(const std::string &raw)
{
    size_t pos = 0;
    while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos]))) ++pos;
    if (pos < raw.size() && raw[pos] == '/') ++pos;
    while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos]))) ++pos;
    const size_t start = pos;
    while (pos < raw.size() && !std::isspace(static_cast<unsigned char>(raw[pos])) &&
           raw[pos] != '/' && raw[pos] != '>') ++pos;
    return local_name(raw.substr(start, pos - start));
}

std::string xml_attr(const std::string &raw, const char *wanted)
{
    size_t pos = 0;
    while (pos < raw.size() && !std::isspace(static_cast<unsigned char>(raw[pos]))) ++pos;
    while (pos < raw.size()) {
        while (pos < raw.size() && (std::isspace(static_cast<unsigned char>(raw[pos])) ||
                                    raw[pos] == '/')) ++pos;
        const size_t key_start = pos;
        while (pos < raw.size() && raw[pos] != '=' &&
               !std::isspace(static_cast<unsigned char>(raw[pos])) && raw[pos] != '/') ++pos;
        if (pos == key_start) break;
        const std::string key = local_name(raw.substr(key_start, pos - key_start));
        while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos]))) ++pos;
        if (pos >= raw.size() || raw[pos] != '=') continue;
        ++pos;
        while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos]))) ++pos;
        const char quote = pos < raw.size() && (raw[pos] == '\'' || raw[pos] == '"') ? raw[pos++] : 0;
        const size_t value_start = pos;
        if (quote) while (pos < raw.size() && raw[pos] != quote) ++pos;
        else while (pos < raw.size() && !std::isspace(static_cast<unsigned char>(raw[pos])) &&
                   raw[pos] != '/') ++pos;
        const std::string value = raw.substr(value_start, pos - value_start);
        if (quote && pos < raw.size()) ++pos;
        if (key == wanted) return value;
    }
    return {};
}

void append_utf8(uint32_t cp, std::string *out)
{
    if (cp < 0x80) out->push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::string decode_entities(const std::string &text)
{
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '&') { out.push_back(text[i++]); continue; }
        const size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 16) { out.push_back(text[i++]); continue; }
        const std::string entity = text.substr(i + 1, semi - i - 1);
        if (entity == "amp") out.push_back('&');
        else if (entity == "lt") out.push_back('<');
        else if (entity == "gt") out.push_back('>');
        else if (entity == "quot") out.push_back('"');
        else if (entity == "apos") out.push_back('\'');
        else if (entity == "nbsp") out.push_back(' ');
        else if (!entity.empty() && entity[0] == '#') {
            char *end = nullptr;
            const int base = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X') ? 16 : 10;
            const char *digits = entity.c_str() + (base == 16 ? 2 : 1);
            const uint32_t cp = static_cast<uint32_t>(std::strtoul(digits, &end, base));
            if (end && *end == '\0' && cp && cp <= 0x10ffff) append_utf8(cp, &out);
        } else out.append(text, i, semi - i + 1);
        i = semi + 1;
    }
    return out;
}

std::string plain_text(const std::string &xml)
{
    std::string out;
    bool in_tag = false;
    bool space = false;
    for (unsigned char c : xml) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; space = true; continue; }
        if (in_tag) continue;
        if (std::isspace(c)) space = !out.empty();
        else {
            if (space && !out.empty()) out.push_back(' ');
            out.push_back(static_cast<char>(c));
            space = false;
        }
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    return decode_entities(out);
}

std::string tag_content(const std::string &xml, const char *name, size_t from = 0, size_t until = std::string::npos)
{
    const std::string open_token = "<" + std::string(name);
    const size_t open = find_ascii_ci(xml, open_token, from);
    if (open == std::string::npos || (until != std::string::npos && open >= until)) return {};
    const size_t open_end = xml.find('>', open + open_token.size());
    if (open_end == std::string::npos) return {};
    const size_t close = find_ascii_ci(xml, "</" + std::string(name), open_end + 1);
    if (close == std::string::npos || (until != std::string::npos && close > until)) return {};
    return plain_text(xml.substr(open_end + 1, close - open_end - 1));
}

std::string extension_for(const std::string &id, const std::string &media_type)
{
    const std::string type = lower_ascii(media_type);
    if (type.find("jpeg") != std::string::npos || type.find("jpg") != std::string::npos) return ".jpg";
    if (type.find("png") != std::string::npos) return ".png";
    if (type.find("bmp") != std::string::npos) return ".bmp";
    if (type.find("gif") != std::string::npos) return ".gif";
    const size_t dot = id.find_last_of('.');
    if (dot != std::string::npos && id.size() - dot <= 6) return lower_ascii(id.substr(dot));
    return ".img";
}

int base64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

bool Fb2Book::open(const char *path)
{
    metadata_memory::Budget budget;
    const int64_t open_started = esp_timer_get_time();
    close();
    if (!path || !path[0]) return false;
    struct stat info = {};
    if (stat(path, &info) != 0 || info.st_size <= 0 ||
        static_cast<uint64_t>(info.st_size) > MAX_FB2_BYTES) {
        _last_error = "FB2 file is empty or too large";
        return false;
    }
    // The index build is temporary, but it needs one contiguous copy of the
    // source XML. Refuse cleanly instead of letting std::string abort when a
    // very large image-heavy FB2 cannot fit in the current PSRAM state.
    const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    constexpr size_t OPEN_RESERVE_BYTES = 2U * 1024U * 1024U;
    if (largest_psram && static_cast<uint64_t>(info.st_size) + OPEN_RESERVE_BYTES > largest_psram) {
        _last_error = "FB2 needs more free PSRAM to build its index";
        ESP_LOGE(TAG, "open memory limit file=%llu largest_psram=%u reserve=%u",
                 static_cast<unsigned long long>(info.st_size),
                 static_cast<unsigned>(largest_psram),
                 static_cast<unsigned>(OPEN_RESERVE_BYTES));
        return false;
    }
    FILE *file = std::fopen(path, "rb");
    if (!file) { _last_error = "Could not open FB2 file"; return false; }
    std::string xml(static_cast<size_t>(info.st_size), '\0');
    const bool read_ok = std::fread(xml.data(), 1, xml.size(), file) == xml.size();
    std::fclose(file);
    if (!read_ok) { _last_error = "Could not read FB2 file"; return false; }
    if ((xml.size() >= 2 && static_cast<unsigned char>(xml[0]) == 0xff &&
         static_cast<unsigned char>(xml[1]) == 0xfe) ||
        (xml.size() >= 2 && static_cast<unsigned char>(xml[0]) == 0xfe &&
         static_cast<unsigned char>(xml[1]) == 0xff)) {
        _last_error = "UTF-16 FB2 is not supported; convert it to UTF-8";
        return false;
    }
    if (find_ascii_ci(xml, "<fictionbook") == std::string::npos &&
        find_ascii_ci(xml, ":fictionbook") == std::string::npos) {
        _last_error = "Not a FictionBook 2 document";
        return false;
    }

    _path = path;
    title = tag_content(xml, "book-title");
    const std::string first = tag_content(xml, "first-name");
    const std::string last = tag_content(xml, "last-name");
    author = first + ((!first.empty() && !last.empty()) ? " " : "") + last;

    std::string cover_id;
    const size_t cover = find_ascii_ci(xml, "<coverpage");
    if (cover != std::string::npos) {
        const size_t cover_end = find_ascii_ci(xml, "</coverpage", cover);
        const size_t image = find_ascii_ci(xml, "<image", cover);
        if (image != std::string::npos && (cover_end == std::string::npos || image < cover_end)) {
            const size_t end = xml.find('>', image + 6);
            if (end != std::string::npos) {
                std::string href = xml_attr(xml.substr(image + 1, end - image - 1), "href");
                if (!href.empty() && href[0] == '#') cover_id = href.substr(1);
            }
        }
    }

    // Index Base64 resources without retaining their encoded payload in RAM.
    size_t scan = 0;
    while ((scan = find_ascii_ci(xml, "<binary", scan)) != std::string::npos) {
        const size_t open_end = xml.find('>', scan + 7);
        if (open_end == std::string::npos) break;
        const size_t close = find_ascii_ci(xml, "</binary", open_end + 1);
        if (close == std::string::npos) break;
        const std::string raw = xml.substr(scan + 1, open_end - scan - 1);
        BinaryItem item;
        item.id = xml_attr(raw, "id");
        item.media_type = xml_attr(raw, "content-type");
        item.start = open_end + 1;
        item.end = close;
        if (!item.id.empty()) {
            item.href = "fb2res/resource-" + std::to_string(_binaries.size()) +
                        extension_for(item.id, item.media_type);
            _binary_hrefs[item.id] = item.href;
            if (item.id == cover_id) _cover_href = item.href;
            _binaries.push_back(std::move(item));
        }
        const size_t close_end = xml.find('>', close + 8);
        scan = close_end == std::string::npos ? close + 8 : close_end + 1;
    }

    // Split each non-notes body into top-level sections. Notes bodies are
    // indexed for popup resolution but are deliberately excluded from spine.
    size_t body_scan = 0;
    int primary_number = 0;
    int note_number = 0;
    while ((body_scan = find_ascii_ci(xml, "<body", body_scan)) != std::string::npos) {
        const size_t body_open_end = xml.find('>', body_scan + 5);
        if (body_open_end == std::string::npos) break;
        const size_t body_close = find_ascii_ci(xml, "</body", body_open_end + 1);
        if (body_close == std::string::npos) break;
        const std::string body_tag = xml.substr(body_scan + 1, body_open_end - body_scan - 1);
        const std::string body_name = lower_ascii(xml_attr(body_tag, "name"));
        const bool notes_body = body_name.find("note") != std::string::npos ||
                                body_name.find("comment") != std::string::npos;

        int depth = 0;
        size_t section_start = std::string::npos;
        size_t pos = body_open_end + 1;
        bool found_top_level = false;
        while (pos < body_close) {
            const size_t tag_start = xml.find('<', pos);
            if (tag_start == std::string::npos || tag_start >= body_close) break;
            const size_t tag_end = xml.find('>', tag_start + 1);
            if (tag_end == std::string::npos || tag_end > body_close) break;
            const std::string raw = xml.substr(tag_start + 1, tag_end - tag_start - 1);
            const std::string name = tag_name(raw);
            const bool closing = !raw.empty() && raw[0] == '/';
            const bool self_closing = !raw.empty() && raw.back() == '/';
            if (name == "section") {
                if (!closing && !self_closing) {
                    if (depth == 0) section_start = tag_start;
                    ++depth;
                } else if (closing && depth > 0) {
                    --depth;
                    if (depth == 0 && section_start != std::string::npos) {
                        Fb2SectionItem section;
                        section.start = section_start;
                        section.end = tag_end + 1;
                        section.in_spine = !notes_body;
                        const size_t first_end = xml.find('>', section_start + 1);
                        const std::string first_tag = xml.substr(section_start + 1,
                            first_end == std::string::npos ? 0 : first_end - section_start - 1);
                        section.id = xml_attr(first_tag, "id");
                        const int number = notes_body ? note_number++ : primary_number++;
                        section.href = std::string("fb2/") + (notes_body ? "note-" : "section-") +
                                       std::to_string(number) + ".xhtml";
                        const size_t title_limit = std::min(section.end, section.start + 8192);
                        section.title = tag_content(xml, "title", section.start, title_limit);
                        if (section.title.empty()) section.title = "Section " + std::to_string(number + 1);
                        const size_t index = _sections.size();
                        if (!budget.add(_sections.size(),sizeof(section),{section.id.size(),section.href.size(),section.title.size()})) { _last_error="Book navigation exceeds metadata memory limit"; return false; }
            _sections.push_back(section);
                        if (section.in_spine) _spine_indices.push_back(index);
                        // FB2 footnotes normally target the section id, but some
                        // generators put the destination id on a child paragraph.
                        // Index every id/name inside the section so the common
                        // XHTML footnote resolver can load the right virtual file.
                        size_t anchor_pos = section.start;
                        while (anchor_pos < section.end) {
                            const size_t anchor_start = xml.find('<', anchor_pos);
                            if (anchor_start == std::string::npos || anchor_start >= section.end) break;
                            if (xml.compare(anchor_start, 4, "<!--") == 0) {
                                const size_t comment_end = xml.find("-->", anchor_start + 4);
                                anchor_pos = comment_end == std::string::npos ? section.end : comment_end + 3;
                                continue;
                            }
                            const size_t anchor_end = xml.find('>', anchor_start + 1);
                            if (anchor_end == std::string::npos || anchor_end >= section.end) break;
                            const std::string anchor_tag = xml.substr(anchor_start + 1,
                                                                      anchor_end - anchor_start - 1);
                            std::string anchor_id = xml_attr(anchor_tag, "id");
                            if (anchor_id.empty()) anchor_id = xml_attr(anchor_tag, "name");
                            if (!anchor_id.empty()) _anchor_hrefs[anchor_id] = section.href;
                            anchor_pos = anchor_end + 1;
                        }
                        found_top_level = true;
                        section_start = std::string::npos;
                    }
                }
            }
            pos = tag_end + 1;
        }
        if (!notes_body && !found_top_level && body_close > body_open_end + 1) {
            Fb2SectionItem section;
            section.id = "body-" + std::to_string(primary_number);
            section.href = "fb2/section-" + std::to_string(primary_number++) + ".xhtml";
            section.title = title.empty() ? "Book" : title;
            section.start = body_open_end + 1;
            section.end = body_close;
            const size_t index = _sections.size();
            if (!budget.add(_sections.size(),sizeof(section),{section.id.size(),section.href.size(),section.title.size()})) { _last_error="Book navigation exceeds metadata memory limit"; return false; }
            _sections.push_back(section);
            _spine_indices.push_back(index);
            _anchor_hrefs[section.id] = section.href;
        }
        const size_t close_end = xml.find('>', body_close + 6);
        body_scan = close_end == std::string::npos ? body_close + 6 : close_end + 1;
    }

    if (title.empty()) {
        const char *slash = std::strrchr(path, '/');
        title = slash ? slash + 1 : path;
        const size_t dot = title.find_last_of('.');
        if (dot != std::string::npos) title.erase(dot);
    }
    if (_spine_indices.empty()) {
        close();
        _last_error = "FB2 document has no readable body";
        return false;
    }
    ESP_LOGI(TAG, "opened title=%s sections=%u notes=%u binaries=%u cover=%s time=%lld ms spiram_free=%u",
             title.c_str(), static_cast<unsigned>(_spine_indices.size()),
             static_cast<unsigned>(_sections.size() - _spine_indices.size()),
             static_cast<unsigned>(_binaries.size()), _cover_href.c_str(),
             (esp_timer_get_time() - open_started) / 1000,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return true;
}

void Fb2Book::close()
{
    _path.clear();
    _cover_href.clear();
    _last_error.clear();
    title.clear();
    author.clear();
    metadata_memory::release(_sections);
    metadata_memory::release(_spine_indices);
    metadata_memory::release(_binaries);
    metadata_memory::release(_anchor_hrefs);
    metadata_memory::release(_binary_hrefs);
}

bool Fb2Book::read_range(uint64_t start, uint64_t end, std::string *out) const
{
    if (!out || start >= end) return false;
    FILE *file = std::fopen(_path.c_str(), "rb");
    if (!file || std::fseek(file, static_cast<long>(start), SEEK_SET) != 0) {
        if (file) std::fclose(file);
        return false;
    }
    out->resize(static_cast<size_t>(end - start));
    const bool ok = std::fread(out->data(), 1, out->size(), file) == out->size();
    std::fclose(file);
    if (!ok) out->clear();
    return ok;
}

std::string Fb2Book::transform_section(const Fb2SectionItem &section,
                                       const std::string &xml) const
{
    std::string out = "<html><body>";
    out.reserve(xml.size() + 128);
    size_t pos = 0;
    while (pos < xml.size()) {
        const size_t start = xml.find('<', pos);
        if (start == std::string::npos) { out.append(xml, pos, std::string::npos); break; }
        out.append(xml, pos, start - pos);
        if (xml.compare(start, 4, "<!--") == 0) {
            const size_t end = xml.find("-->", start + 4);
            pos = end == std::string::npos ? xml.size() : end + 3;
            continue;
        }
        const size_t end = xml.find('>', start + 1);
        if (end == std::string::npos) break;
        const std::string raw = xml.substr(start + 1, end - start - 1);
        const std::string name = tag_name(raw);
        const bool closing = !raw.empty() && raw[0] == '/';
        const bool self_closing = !raw.empty() && raw.back() == '/';
        auto simple = [&](const char *mapped) {
            out += closing ? "</" : "<";
            out += mapped;
            out += self_closing && !closing ? "/>" : ">";
        };

        if (name == "image" && !closing) {
            std::string href = xml_attr(raw, "href");
            if (!href.empty() && href[0] == '#') href.erase(0, 1);
            const auto found = _binary_hrefs.find(href);
            if (found != _binary_hrefs.end()) out += "<img src=\"../" + found->second + "\"/>";
        } else if (name == "a") {
            if (closing) out += "</a>";
            else {
                std::string href = xml_attr(raw, "href");
                std::string type = xml_attr(raw, "type");
                if (!href.empty() && href[0] == '#') {
                    const auto target = _anchor_hrefs.find(href.substr(1));
                    if (target != _anchor_hrefs.end()) {
                        const size_t slash = target->second.find_last_of('/');
                        href = target->second.substr(slash == std::string::npos ? 0 : slash + 1) + href;
                    }
                }
                out += "<a";
                if (!href.empty()) out += " href=\"" + href + "\"";
                if (!type.empty()) out += " type=\"" + type + "\"";
                out += ">";
            }
        } else if (name == "title") simple("h2");
        else if (name == "subtitle") simple("h3");
        else if (name == "section") {
            if (closing) out += "</div>";
            else {
                out += "<div";
                const std::string id = xml_attr(raw, "id");
                if (!id.empty()) out += " id=\"" + id + "\"";
                out += ">";
            }
        } else if (name == "empty-line") out += "<br/>";
        else if (name == "v" || name == "text-author" || name == "date") simple("p");
        else if (name == "poem" || name == "cite" || name == "epigraph") simple("blockquote");
        else if (name == "stanza") simple("div");
        else if (name == "emphasis") simple("em");
        else if (name == "p") {
            if (closing) out += "</p>";
            else {
                out += "<p";
                const std::string id = xml_attr(raw, "id");
                if (!id.empty()) out += " id=\"" + id + "\"";
                out += ">";
            }
        } else if (name == "strong" || name == "sup" || name == "sub" ||
                 name == "code" || name == "strikethrough") simple(name == "strikethrough" ? "span" : name.c_str());
        pos = end + 1;
    }
    out += "</body></html>";
    (void)section;
    return out;
}

bool Fb2Book::read_section_to_string(const std::string &href, std::string *out) const
{
    if (!out) return false;
    for (const auto &section : _sections) {
        if (section.href != href) continue;
        std::string xml;
        if (!read_range(section.start, section.end, &xml)) return false;
        *out = transform_section(section, xml);
        return true;
    }
    return false;
}

bool Fb2Book::extract_resource_to_file(const std::string &href, const std::string &out_path) const
{
    const BinaryItem *item = nullptr;
    for (const auto &candidate : _binaries) {
        if (candidate.href == href) { item = &candidate; break; }
    }
    if (!item) return false;
    FILE *input = std::fopen(_path.c_str(), "rb");
    FILE *output = input ? std::fopen(out_path.c_str(), "wb") : nullptr;
    if (!input || !output || std::fseek(input, static_cast<long>(item->start), SEEK_SET) != 0) {
        if (output) std::fclose(output);
        if (input) std::fclose(input);
        return false;
    }
    uint32_t accumulator = 0;
    int bits = 0;
    bool ok = true;
    uint64_t remaining = item->end - item->start;
    uint8_t buffer[2048];
    while (remaining && ok) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(buffer)));
        const size_t count = std::fread(buffer, 1, take, input);
        if (count != take) { ok = false; break; }
        remaining -= count;
        for (size_t i = 0; i < count; ++i) {
            const unsigned char c = buffer[i];
            if (std::isspace(c)) continue;
            if (c == '=') { remaining = 0; break; }
            const int value = base64_value(c);
            if (value < 0) { ok = false; break; }
            accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                const uint8_t byte = static_cast<uint8_t>((accumulator >> bits) & 0xff);
                if (std::fwrite(&byte, 1, 1, output) != 1) { ok = false; break; }
            }
        }
    }
    if (std::fclose(output) != 0) ok = false;
    std::fclose(input);
    if (!ok) std::remove(out_path.c_str());
    return ok;
}

bool Fb2Book::decode_resource(const std::string &href,
                              std::vector<uint8_t> *out,
                              size_t max_bytes,
                              bool allow_prefix) const
{
    if (!out || max_bytes == 0) return false;
    out->clear();
    const BinaryItem *item = nullptr;
    for (const auto &candidate : _binaries) {
        if (candidate.href == href) { item = &candidate; break; }
    }
    if (!item) return false;

    FILE *input = std::fopen(_path.c_str(), "rb");
    if (!input || std::fseek(input, static_cast<long>(item->start), SEEK_SET) != 0) {
        if (input) std::fclose(input);
        return false;
    }

    out->reserve(std::min<size_t>(max_bytes, 128 * 1024));
    uint32_t accumulator = 0;
    int bits = 0;
    bool ok = true;
    bool complete = false;
    uint64_t remaining = item->end - item->start;
    uint8_t buffer[2048];
    while (remaining && ok && !complete) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(buffer)));
        const size_t count = std::fread(buffer, 1, take, input);
        if (count != take) { ok = false; break; }
        remaining -= count;
        for (size_t i = 0; i < count; ++i) {
            const unsigned char c = buffer[i];
            if (std::isspace(c)) continue;
            if (c == '=') { complete = true; break; }
            const int value = base64_value(c);
            if (value < 0) { ok = false; break; }
            accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (out->size() >= max_bytes) {
                    complete = allow_prefix;
                    ok = allow_prefix;
                    break;
                }
                out->push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
            }
        }
    }
    std::fclose(input);
    if (!allow_prefix && remaining != 0 && !complete) ok = false;
    if (!ok || out->empty()) out->clear();
    return ok && !out->empty();
}

bool Fb2Book::read_resource_prefix(const std::string &href,
                                   std::vector<uint8_t> *out,
                                   size_t max_bytes) const
{
    return decode_resource(href, out, max_bytes, true);
}

bool Fb2Book::read_resource_to_memory(const std::string &href,
                                      std::vector<uint8_t> *out,
                                      uint32_t max_size) const
{
    return decode_resource(href, out, max_size, false);
}
