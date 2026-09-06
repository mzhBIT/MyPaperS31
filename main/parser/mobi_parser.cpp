#include "parser/mobi_parser.h"
#include "parser/epub_html.h"
#include "storage/app_paths.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include <unordered_map>

namespace {

static const char *TAG = "MOBI";
static constexpr uint64_t TARGET_SECTION_BYTES = 96 * 1024;
static constexpr uint64_t MIN_SECTION_BYTES = 4 * 1024;
static constexpr size_t MAX_RECORD_BYTES = 2 * 1024 * 1024;
static constexpr uint32_t META_MAGIC = 0x4d424331; // MBC1
static constexpr uint32_t META_VERSION = 2;

uint16_t read_be16(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    });
    return value;
}

void append_utf8(uint32_t cp, std::string *out)
{
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::string cp1252_to_utf8(const uint8_t *data, size_t size)
{
    static const uint16_t high[32] = {
        0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
        0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
        0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
        0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
    };
    std::string out;
    out.reserve(size + size / 4);
    for (size_t i = 0; i < size; ++i) {
        uint32_t cp = data[i];
        if (cp >= 0x80 && cp <= 0x9f) cp = high[cp - 0x80];
        append_utf8(cp, &out);
    }
    return out;
}

bool palmdoc_decompress(const uint8_t *input, size_t input_size, std::string *output)
{
    if (!output) return false;
    output->clear();
    output->reserve(4096 + 32);
    size_t pos = 0;
    while (pos < input_size) {
        uint8_t c = input[pos++];
        if (c >= 1 && c <= 8) {
            if (pos + c > input_size) return false;
            output->append(reinterpret_cast<const char *>(input + pos), c);
            pos += c;
        } else if (c <= 0x7f) {
            output->push_back(static_cast<char>(c));
        } else if (c >= 0xc0) {
            output->push_back(' ');
            output->push_back(static_cast<char>(c ^ 0x80));
        } else {
            if (pos >= input_size) return false;
            uint16_t pair = static_cast<uint16_t>((static_cast<uint16_t>(c) << 8) | input[pos++]);
            size_t distance = (pair >> 3) & 0x7ff;
            size_t length = (pair & 7) + 3;
            if (distance == 0 || distance > output->size()) return false;
            for (size_t i = 0; i < length; ++i) {
                output->push_back((*output)[output->size() - distance]);
            }
        }
        if (output->size() > MAX_RECORD_BYTES) return false;
    }
    return true;
}

size_t trailing_entry_size(const uint8_t *data, size_t size)
{
    uint32_t value = 0;
    unsigned shift = 0;
    for (size_t i = 0; i < 4 && i < size; ++i) {
        uint8_t c = data[size - 1 - i];
        value |= static_cast<uint32_t>(c & 0x7f) << shift;
        if (c & 0x80) return value <= size ? value : 0;
        shift += 7;
    }
    return 0;
}

size_t trailing_data_size(const uint8_t *data, size_t size, uint16_t flags)
{
    size_t trailing = 0;
    uint16_t entries = flags >> 1;
    while (entries) {
        if (entries & 1) {
            size_t available = size > trailing ? size - trailing : 0;
            size_t entry = trailing_entry_size(data, available);
            if (entry == 0 || entry > available) return trailing;
            trailing += entry;
        }
        entries >>= 1;
    }
    if ((flags & 1) && size > trailing) {
        size_t overlap = (data[size - trailing - 1] & 3) + 1;
        if (overlap <= size - trailing) trailing += overlap;
    }
    return trailing;
}

std::string strip_tags(const std::string &input)
{
    std::string out;
    bool in_tag = false;
    bool space = false;
    for (unsigned char c : input) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            space = true;
            continue;
        }
        if (in_tag) continue;
        if (std::isspace(c)) {
            space = !out.empty();
        } else {
            if (space && !out.empty()) out.push_back(' ');
            out.push_back(static_cast<char>(c));
            space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string first_heading(const std::string &html)
{
    std::string lower = lower_ascii(html);
    for (int level = 1; level <= 3; ++level) {
        std::string token = "<h" + std::to_string(level);
        size_t start = lower.find(token);
        if (start == std::string::npos) continue;
        start = lower.find('>', start + token.size());
        if (start == std::string::npos) continue;
        size_t end = lower.find("</h", start + 1);
        if (end == std::string::npos || end - start > 512) continue;
        std::string heading = strip_tags(html.substr(start + 1, end - start - 1));
        if (!heading.empty()) return heading;
    }
    return std::string();
}

bool parse_decimal_attr(const std::string &tag, const char *attribute, uint32_t *value)
{
    std::string lower = lower_ascii(tag);
    size_t pos = lower.find(attribute);
    if (pos == std::string::npos) return false;
    pos += std::strlen(attribute);
    while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
    if (pos >= tag.size() || tag[pos] != '=') return false;
    ++pos;
    while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
    char quote = pos < tag.size() && (tag[pos] == '\'' || tag[pos] == '"') ? tag[pos++] : 0;
    size_t start = pos;
    while (pos < tag.size() && std::isdigit(static_cast<unsigned char>(tag[pos]))) ++pos;
    if (pos == start || (quote && (pos >= tag.size() || tag[pos] != quote))) return false;
    *value = static_cast<uint32_t>(std::strtoul(tag.substr(start, pos - start).c_str(), nullptr, 10));
    return true;
}

int base32_digit(unsigned char c)
{
    c = static_cast<unsigned char>(std::toupper(c));
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'V') return c - 'A' + 10;
    return -1;
}

} // namespace

struct MobiBook::HuffState {
    struct Code {
        uint8_t length = 0;
        bool terminal = false;
        uint32_t max_code = 0;
    };
    struct Phrase {
        std::string bytes;
        bool ready = false;
        bool expanding = false;
    };

    Code first[256] = {};
    uint32_t min_code[33] = {};
    uint32_t max_code[33] = {};
    std::vector<Phrase> phrases;

    static uint32_t peek32(const uint8_t *data, size_t size, size_t bit_position)
    {
        size_t byte_position = bit_position / 8;
        unsigned shift = static_cast<unsigned>(bit_position & 7);
        uint64_t window = 0;
        for (size_t i = 0; i < 8; ++i) {
            window <<= 8;
            if (byte_position + i < size) window |= data[byte_position + i];
        }
        window <<= shift;
        return static_cast<uint32_t>(window >> 32);
    }

    bool expand_phrase(size_t index, std::string *out, unsigned depth)
    {
        if (!out || index >= phrases.size() || depth > 64) return false;
        Phrase &phrase = phrases[index];
        if (phrase.ready) {
            out->append(phrase.bytes);
            return out->size() <= MAX_RECORD_BYTES;
        }
        if (phrase.expanding) return false;
        phrase.expanding = true;
        std::string expanded;
        bool ok = decode(reinterpret_cast<const uint8_t *>(phrase.bytes.data()), phrase.bytes.size(),
                         &expanded, depth + 1);
        phrase.expanding = false;
        if (!ok) return false;
        phrase.bytes = std::move(expanded);
        phrase.ready = true;
        out->append(phrase.bytes);
        return out->size() <= MAX_RECORD_BYTES;
    }

    bool decode(const uint8_t *data, size_t size, std::string *out, unsigned depth = 0)
    {
        if (!data || !out || depth > 64) return false;
        out->clear();
        out->reserve(std::min<size_t>(MAX_RECORD_BYTES, size * 3 + 64));
        const size_t bit_count = size * 8;
        size_t bit_position = 0;
        while (bit_position < bit_count) {
            uint32_t bits = peek32(data, size, bit_position);
            Code code = first[bits >> 24];
            unsigned length = code.length;
            uint32_t upper = code.max_code;
            if (length == 0 || length > 32) return false;
            if (!code.terminal) {
                while (length <= 32 && bits < min_code[length]) ++length;
                if (length > 32) return false;
                upper = max_code[length];
            }
            if (bit_position + length > bit_count) break;
            bit_position += length;
            uint32_t phrase_index = length == 32 ? upper - bits : (upper - bits) >> (32 - length);
            if (!expand_phrase(phrase_index, out, depth)) return false;
        }
        return true;
    }
};

bool MobiBook::open(const char *path)
{
    int64_t started = esp_timer_get_time();
    close();
    _last_error.clear();
    if (!path || !path[0]) return false;
    _path = path;
    _file = std::fopen(_path.c_str(), "rb");
    if (!_file) {
        _last_error = "Could not open MOBI file";
        close();
        return false;
    }
    // Text and resource records are usually adjacent. Keeping one buffered
    // stream for the lifetime of the reader avoids thousands of FAT open/close
    // operations while building the first metadata index.
    setvbuf(_file, nullptr, _IOFBF, 32 * 1024);
    if (!parse_container()) {
        if (_last_error.empty()) _last_error = "Invalid MOBI container";
        close();
        return false;
    }
    bool metadata_cached = load_metadata_cache();
    if (!metadata_cached && !build_text_index()) {
        if (_last_error.empty()) _last_error = "Could not decode MOBI text";
        std::string error = _last_error;
        close();
        _last_error = error;
        return false;
    }
    if (!metadata_cached) save_metadata_cache();
    ESP_LOGI(TAG, "opened title=%s records=%u text_records=%u sections=%u compression=%u version=%lu meta_cache=%d time=%lld ms spiram_free=%u",
             title.c_str(), static_cast<unsigned>(_records.size()), _text_record_count,
             static_cast<unsigned>(_sections.size()), _compression,
             static_cast<unsigned long>(_mobi_version), metadata_cached ? 1 : 0,
             (esp_timer_get_time() - started) / 1000,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return true;
}

std::string MobiBook::metadata_cache_path() const
{
    uint32_t hash = 2166136261u;
    for (unsigned char c : _path) {
        hash ^= c;
        hash *= 16777619u;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "/%08lx.meta", static_cast<unsigned long>(hash));
    return std::string(app_paths::mobi_metadata_dir()) + name;
}

bool MobiBook::load_metadata_cache()
{
    struct stat source = {};
    if (stat(_path.c_str(), &source) != 0) return false;
    FILE *fp = std::fopen(metadata_cache_path().c_str(), "rb");
    if (!fp) return false;
    auto read_value = [fp](auto *value) {
        return std::fread(value, 1, sizeof(*value), fp) == sizeof(*value);
    };
    uint32_t magic = 0, version = 0, text_count = 0, section_count = 0, cache_format = 0;
    uint64_t file_size = 0;
    int64_t modified = 0;
    uint64_t decoded_length = 0;
    bool ok = read_value(&magic) && read_value(&version) && read_value(&file_size) &&
              read_value(&modified) && read_value(&decoded_length) && read_value(&text_count) &&
              magic == META_MAGIC && (version == 1 || version == META_VERSION) &&
              file_size == static_cast<uint64_t>(source.st_size) &&
              modified == static_cast<int64_t>(source.st_mtime) &&
              text_count == _text_record_count;
    if (ok && version >= 2) ok = read_value(&cache_format);
    // Version 1 is the original contiguous KF7 cache. Keep accepting it so a
    // firmware upgrade does not force large existing MOBI books to re-index.
    if (ok) ok = cache_format == (_is_kf8 ? 1u : 0u) && (version >= 2 || !_is_kf8);
    std::vector<TextRecord> text_records;
    if (ok) {
        text_records.reserve(text_count);
        for (uint32_t i = 0; i < text_count; ++i) {
            TextRecord item;
            if (!read_value(&item.record_index) || !read_value(&item.start) || !read_value(&item.length) ||
                item.record_index == 0 || item.record_index >= _records.size()) {
                ok = false;
                break;
            }
            text_records.push_back(item);
        }
    }
    if (ok) ok = read_value(&section_count) && section_count > 0 && section_count <= 100000;
    std::vector<MobiSectionItem> sections;
    std::vector<MobiTocItem> toc;
    if (ok) {
        sections.reserve(section_count);
        toc.reserve(section_count);
        for (uint32_t i = 0; i < section_count; ++i) {
            uint64_t start = 0, end = 0;
            uint32_t label_size = 0;
            if (!read_value(&start) || !read_value(&end) || !read_value(&label_size) ||
                start >= end || end > decoded_length || label_size > 4096) {
                ok = false;
                break;
            }
            std::string label(label_size, '\0');
            if (label_size && std::fread(label.data(), 1, label_size, fp) != label_size) {
                ok = false;
                break;
            }
            MobiSectionItem section;
            section.id = "section-" + std::to_string(i);
            section.href = section.id + ".html";
            section.start = start;
            section.end = end;
            section.uncompressed_size = static_cast<uint32_t>(
                std::min<uint64_t>(0xffffffffu, end - start));
            sections.push_back(section);
            MobiTocItem entry;
            entry.title = label.empty() ? "Section " + std::to_string(i + 1) : std::move(label);
            entry.href = section.href;
            entry.spine_index = static_cast<int>(i);
            toc.push_back(std::move(entry));
        }
    }
    std::vector<Kf8Part> kf8_parts;
    if (ok && _is_kf8) {
        kf8_parts.reserve(section_count);
        for (uint32_t i = 0; i < section_count; ++i) {
            Kf8Part part;
            uint32_t fragment_count = 0;
            if (!read_value(&part.skeleton_start) || !read_value(&part.skeleton_length) ||
                !read_value(&part.assembled_length) || !read_value(&fragment_count) ||
                part.skeleton_start > decoded_length ||
                part.skeleton_length > decoded_length - part.skeleton_start ||
                part.assembled_length < part.skeleton_length || fragment_count > 100000) {
                ok = false;
                break;
            }
            uint64_t current_length = part.skeleton_length;
            part.fragments.reserve(fragment_count);
            for (uint32_t n = 0; n < fragment_count; ++n) {
                Kf8Fragment fragment;
                if (!read_value(&fragment.insert_offset) || !read_value(&fragment.raw_start) ||
                    !read_value(&fragment.length) || fragment.insert_offset > current_length ||
                    fragment.raw_start > decoded_length ||
                    fragment.length > decoded_length - fragment.raw_start ||
                    current_length + fragment.length > 0xffffffffu) {
                    ok = false;
                    break;
                }
                current_length += fragment.length;
                part.fragments.push_back(fragment);
            }
            if (!ok || current_length != part.assembled_length) {
                ok = false;
                break;
            }
            kf8_parts.push_back(std::move(part));
        }
    }
    std::fclose(fp);
    if (!ok || text_records.size() != text_count || sections.size() != section_count ||
        (_is_kf8 && kf8_parts.size() != section_count)) return false;
    _decoded_text_length = decoded_length;
    _text_records.swap(text_records);
    _sections.swap(sections);
    _toc.swap(toc);
    _kf8_parts.swap(kf8_parts);
    ESP_LOGI(TAG, "metadata cache loaded text_records=%u sections=%u",
             static_cast<unsigned>(_text_records.size()), static_cast<unsigned>(_sections.size()));
    return true;
}

void MobiBook::save_metadata_cache() const
{
    if (_text_records.empty() || _sections.empty()) return;
    struct stat source = {};
    if (stat(_path.c_str(), &source) != 0) return;
    app_paths::ensure_dir(app_paths::mobi_metadata_dir());
    std::string path = metadata_cache_path();
    std::string temporary = path + ".tmp";
    FILE *fp = std::fopen(temporary.c_str(), "wb");
    if (!fp) return;
    auto write_value = [fp](const auto &value) {
        return std::fwrite(&value, 1, sizeof(value), fp) == sizeof(value);
    };
    uint64_t file_size = static_cast<uint64_t>(source.st_size);
    int64_t modified = static_cast<int64_t>(source.st_mtime);
    uint32_t text_count = static_cast<uint32_t>(_text_records.size());
    uint32_t section_count = static_cast<uint32_t>(_sections.size());
    const uint32_t cache_format = _is_kf8 ? 1u : 0u;
    bool ok = write_value(META_MAGIC) && write_value(META_VERSION) && write_value(file_size) &&
              write_value(modified) && write_value(_decoded_text_length) && write_value(text_count) &&
              write_value(cache_format);
    for (const auto &item : _text_records) {
        if (!ok) break;
        ok = write_value(item.record_index) && write_value(item.start) && write_value(item.length);
    }
    if (ok) ok = write_value(section_count);
    for (size_t i = 0; ok && i < _sections.size(); ++i) {
        uint32_t label_size = i < _toc.size() ? static_cast<uint32_t>(_toc[i].title.size()) : 0;
        ok = write_value(_sections[i].start) && write_value(_sections[i].end) && write_value(label_size) &&
             (label_size == 0 || std::fwrite(_toc[i].title.data(), 1, label_size, fp) == label_size);
    }
    if (_is_kf8 && _kf8_parts.size() != _sections.size()) ok = false;
    for (const auto &part : _kf8_parts) {
        if (!ok) break;
        const uint32_t fragment_count = static_cast<uint32_t>(part.fragments.size());
        ok = write_value(part.skeleton_start) && write_value(part.skeleton_length) &&
             write_value(part.assembled_length) && write_value(fragment_count);
        for (const auto &fragment : part.fragments) {
            if (!ok) break;
            ok = write_value(fragment.insert_offset) && write_value(fragment.raw_start) &&
                 write_value(fragment.length);
        }
    }
    if (std::fclose(fp) != 0) ok = false;
    if (ok) {
        std::remove(path.c_str());
        if (std::rename(temporary.c_str(), path.c_str()) != 0) std::remove(temporary.c_str());
    } else {
        std::remove(temporary.c_str());
    }
}

void MobiBook::close()
{
    if (_file) {
        std::fclose(_file);
        _file = nullptr;
    }
    title.clear();
    author.clear();
    _path.clear();
    _cover_href.clear();
    _records.clear();
    _text_records.clear();
    _sections.clear();
    _toc.clear();
    _compression = 0;
    _encryption = 0;
    _text_record_count = 0;
    _extra_data_flags = 0;
    _encoding = 65001;
    _mobi_version = 0;
    _is_kf8 = false;
    _fdst_record_index = 0xffffffffu;
    _fdst_flow_count = 0;
    _fragment_index = 0xffffffffu;
    _skeleton_index = 0xffffffffu;
    _first_image_record = 0xffffffffu;
    _huff_record_index = 0xffffffffu;
    _huff_record_count = 0;
    _decoded_text_length = 0;
    _kf8_parts.clear();
    _huff.reset();
}

bool MobiBook::parse_container()
{
    FILE *fp = _file;
    if (!fp) {
        _last_error = "Could not open MOBI file";
        return false;
    }
    if (std::fseek(fp, 0, SEEK_SET) != 0) {
        _last_error = "Could not seek MOBI file";
        return false;
    }
    struct stat st = {};
    bool ok = stat(_path.c_str(), &st) == 0 && st.st_size >= 86;
    uint8_t header[78] = {};
    if (ok) ok = std::fread(header, 1, sizeof(header), fp) == sizeof(header);
    if (!ok) {
        _last_error = "Truncated Palm database header";
        return false;
    }
    uint16_t count = read_be16(header + 76);
    uint64_t file_size = static_cast<uint64_t>(st.st_size);
    uint64_t table_end = 78u + static_cast<uint64_t>(count) * 8u;
    if (count == 0 || table_end > file_size || file_size > 0xffffffffu) {
        _last_error = "Invalid Palm database record table";
        ESP_LOGE(TAG, "invalid record table count=%u table_end=%llu file_size=%llu",
                 count, static_cast<unsigned long long>(table_end),
                 static_cast<unsigned long long>(file_size));
        return false;
    }
    std::vector<uint32_t> offsets(count);
    uint8_t entry[8];
    for (uint16_t i = 0; i < count; ++i) {
        if (std::fread(entry, 1, sizeof(entry), fp) != sizeof(entry)) {
            _last_error = "Truncated Palm database record table";
            return false;
        }
        offsets[i] = read_be32(entry);
        // Some MOBI generators retain empty records. They are represented by
        // equal adjacent offsets, and an empty final record may point exactly
        // at EOF. Both are valid PDB layouts; only a decreasing or past-EOF
        // offset is structurally invalid.
        if (offsets[i] > file_size || (i && offsets[i] < offsets[i - 1])) {
            _last_error = "Invalid Palm database record offset";
            ESP_LOGE(TAG,
                     "invalid record offset index=%u offset=%lu previous=%lu file_size=%llu count=%u",
                     i, static_cast<unsigned long>(offsets[i]),
                     static_cast<unsigned long>(i ? offsets[i - 1] : 0),
                     static_cast<unsigned long long>(file_size), count);
            return false;
        }
    }
    _records.resize(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t end = i + 1 < count ? offsets[i + 1] : static_cast<uint32_t>(file_size);
        _records[i] = {offsets[i], end - offsets[i]};
    }
    std::vector<uint8_t> record_zero;
    return read_record(0, &record_zero) && parse_record_zero(record_zero);
}

bool MobiBook::parse_record_zero(const std::vector<uint8_t> &record)
{
    if (record.size() < 132 || std::memcmp(record.data() + 16, "MOBI", 4) != 0) {
        _last_error = "Palm database is not a MOBI book";
        return false;
    }
    _compression = read_be16(record.data());
    _text_record_count = read_be16(record.data() + 8);
    _encryption = read_be16(record.data() + 12);
    uint32_t header_length = read_be32(record.data() + 20);
    _encoding = read_be32(record.data() + 28);
    // MOBI version is at absolute record-zero offset 0x24. The old code read
    // 0x68 (minimum reader version), which happens to look like 6 for many
    // MOBI files but cannot reliably distinguish KF8/AZW3.
    _mobi_version = record.size() >= 40 ? read_be32(record.data() + 36) : 0;
    _is_kf8 = _mobi_version >= 8;
    _first_image_record = record.size() >= 112 ? read_be32(record.data() + 108) : 0xffffffffu;
    _huff_record_index = record.size() >= 116 ? read_be32(record.data() + 112) : 0xffffffffu;
    _huff_record_count = record.size() >= 120 ? read_be32(record.data() + 116) : 0;
    if (record.size() >= 244 && header_length >= 228) _extra_data_flags = read_be16(record.data() + 242);

    if (_encryption != 0) {
        _last_error = "DRM-protected MOBI is not supported";
        return false;
    }
    if (_compression != 1 && _compression != 2 && _compression != 17480) {
        _last_error = "Unsupported MOBI compression";
        return false;
    }
    if (_is_kf8) {
        if (record.size() < 256 || header_length < 240) {
            _last_error = "Truncated KF8 MOBI header";
            return false;
        }
        _fdst_record_index = read_be32(record.data() + 0xc0);
        _fdst_flow_count = read_be32(record.data() + 0xc4);
        _fragment_index = read_be32(record.data() + 0xf8);
        _skeleton_index = read_be32(record.data() + 0xfc);
    }
    if (_text_record_count == 0 || static_cast<size_t>(_text_record_count) >= _records.size()) {
        _last_error = "Invalid MOBI text record count";
        return false;
    }

    uint32_t name_offset = read_be32(record.data() + 84);
    uint32_t name_length = read_be32(record.data() + 88);
    if (name_length && name_offset <= record.size() && name_length <= record.size() - name_offset) {
        const uint8_t *name = record.data() + name_offset;
        title = _encoding == 1252 ? cp1252_to_utf8(name, name_length)
                                  : std::string(reinterpret_cast<const char *>(name), name_length);
    }

    uint32_t cover_offset = 0xffffffffu;
    bool has_exth = (read_be32(record.data() + 128) & 0x40u) != 0;
    size_t exth = 16u + header_length;
    if (has_exth && exth + 12 <= record.size() && std::memcmp(record.data() + exth, "EXTH", 4) == 0) {
        uint32_t exth_length = read_be32(record.data() + exth + 4);
        uint32_t exth_count = read_be32(record.data() + exth + 8);
        size_t pos = exth + 12;
        size_t limit = std::min(record.size(), exth + static_cast<size_t>(exth_length));
        for (uint32_t i = 0; i < exth_count && pos + 8 <= limit; ++i) {
            uint32_t type = read_be32(record.data() + pos);
            uint32_t length = read_be32(record.data() + pos + 4);
            if (length < 8 || length > limit - pos) break;
            const uint8_t *data = record.data() + pos + 8;
            size_t data_size = length - 8;
            if (type == 100 && author.empty()) {
                author = _encoding == 1252 ? cp1252_to_utf8(data, data_size)
                                           : std::string(reinterpret_cast<const char *>(data), data_size);
            } else if (type == 503 && data_size) {
                title = _encoding == 1252 ? cp1252_to_utf8(data, data_size)
                                          : std::string(reinterpret_cast<const char *>(data), data_size);
            } else if (type == 201 && data_size == 4) {
                cover_offset = read_be32(data);
            }
            pos += length;
        }
    }
    while (!title.empty() && (title.back() == '\0' || title.back() == ' ')) title.pop_back();
    if (title.empty()) title = "MOBI Book";
    if (cover_offset != 0xffffffffu && _first_image_record != 0xffffffffu) {
        _cover_href = resource_href(cover_offset + 1);
    }
    if (_compression == 17480 && !load_huff_dictionary()) return false;
    return true;
}

bool MobiBook::load_huff_dictionary()
{
    if (_huff_record_index == 0xffffffffu || _huff_record_count < 2 ||
        static_cast<uint64_t>(_huff_record_index) + _huff_record_count > _records.size()) {
        _last_error = "Invalid HUFF/CDIC record range";
        return false;
    }
    auto state = std::make_shared<HuffState>();
    std::vector<uint8_t> huff;
    if (!read_record(_huff_record_index, &huff) || huff.size() < 24 ||
        std::memcmp(huff.data(), "HUFF\0\0\0\x18", 8) != 0) {
        _last_error = "Invalid HUFF table";
        return false;
    }
    uint32_t first_offset = read_be32(huff.data() + 8);
    uint32_t second_offset = read_be32(huff.data() + 12);
    if (first_offset > huff.size() || 1024 > huff.size() - first_offset ||
        second_offset > huff.size() || 256 > huff.size() - second_offset) {
        _last_error = "Truncated HUFF table";
        return false;
    }
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t value = read_be32(huff.data() + first_offset + i * 4);
        unsigned length = value & 0x1f;
        bool terminal = (value & 0x80) != 0;
        uint32_t upper = value >> 8;
        if (length == 0 || length > 32 || (length <= 8 && !terminal)) {
            _last_error = "Corrupt HUFF lookup entry";
            return false;
        }
        if (length < 32) upper = static_cast<uint32_t>(
            ((static_cast<uint64_t>(upper) + 1u) << (32 - length)) - 1u);
        state->first[i] = {static_cast<uint8_t>(length), terminal, upper};
    }
    for (unsigned length = 1; length <= 32; ++length) {
        uint32_t low = read_be32(huff.data() + second_offset + (length - 1) * 8);
        uint32_t high = read_be32(huff.data() + second_offset + (length - 1) * 8 + 4);
        state->min_code[length] = length == 32 ? low : low << (32 - length);
        state->max_code[length] = length == 32 ? high : static_cast<uint32_t>(
            ((static_cast<uint64_t>(high) + 1u) << (32 - length)) - 1u);
    }

    uint32_t total_phrases = 0;
    for (uint32_t record_number = 1; record_number < _huff_record_count; ++record_number) {
        std::vector<uint8_t> cdic;
        if (!read_record(_huff_record_index + record_number, &cdic) || cdic.size() < 16 ||
            std::memcmp(cdic.data(), "CDIC\0\0\0\x10", 8) != 0) {
            _last_error = "Invalid CDIC table";
            return false;
        }
        uint32_t phrases = read_be32(cdic.data() + 8);
        uint32_t bits = read_be32(cdic.data() + 12);
        if (bits > 16 || phrases == 0 || phrases > 1000000) {
            _last_error = "Invalid CDIC phrase count";
            return false;
        }
        if (total_phrases == 0) total_phrases = phrases;
        uint32_t remaining = total_phrases > state->phrases.size()
                                 ? total_phrases - static_cast<uint32_t>(state->phrases.size())
                                 : 0;
        uint32_t count = std::min<uint32_t>(1u << bits, remaining);
        if (16u + count * 2u > cdic.size()) {
            _last_error = "Truncated CDIC offsets";
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t offset = read_be16(cdic.data() + 16 + i * 2);
            size_t length_position = 16u + offset;
            if (length_position + 2 > cdic.size()) {
                _last_error = "Invalid CDIC phrase offset";
                return false;
            }
            uint16_t encoded_length = read_be16(cdic.data() + length_position);
            size_t length = encoded_length & 0x7fff;
            if (length_position + 2 + length > cdic.size()) {
                _last_error = "Truncated CDIC phrase";
                return false;
            }
            HuffState::Phrase phrase;
            phrase.bytes.assign(reinterpret_cast<const char *>(cdic.data() + length_position + 2), length);
            phrase.ready = (encoded_length & 0x8000) != 0;
            state->phrases.push_back(std::move(phrase));
        }
    }
    if (state->phrases.empty()) {
        _last_error = "Empty CDIC dictionary";
        return false;
    }
    _huff = std::move(state);
    ESP_LOGI(TAG, "HUFF/CDIC loaded records=%lu phrases=%u",
             static_cast<unsigned long>(_huff_record_count),
             static_cast<unsigned>(_huff->phrases.size()));
    return true;
}

bool MobiBook::read_record(size_t index, std::vector<uint8_t> *out) const
{
    if (!out || !_file || index >= _records.size()) return false;
    const Record &record = _records[index];
    if (record.size == 0 || record.size > MAX_RECORD_BYTES) return false;
    bool ok = std::fseek(_file, static_cast<long>(record.offset), SEEK_SET) == 0;
    if (ok) {
        out->resize(record.size);
        ok = std::fread(out->data(), 1, out->size(), _file) == out->size();
    }
    if (!ok) out->clear();
    return ok;
}

bool MobiBook::decode_text_record(size_t index, std::string *out) const
{
    std::vector<uint8_t> data;
    if (!read_record(index, &data) || data.empty()) return false;
    size_t trailing = trailing_data_size(data.data(), data.size(), _extra_data_flags);
    if (trailing > data.size()) return false;
    size_t payload = data.size() - trailing;
    std::string decoded;
    bool ok = false;
    if (_compression == 1) {
        decoded.assign(reinterpret_cast<const char *>(data.data()), payload);
        ok = true;
    } else if (_compression == 2) {
        ok = palmdoc_decompress(data.data(), payload, &decoded);
    } else if (_compression == 17480 && _huff) {
        ok = _huff->decode(data.data(), payload, &decoded);
    }
    if (!ok) return false;
    *out = _encoding == 1252
               ? cp1252_to_utf8(reinterpret_cast<const uint8_t *>(decoded.data()), decoded.size())
               : std::move(decoded);
    return true;
}

bool MobiBook::build_kf8_index()
{
    struct TagDefinition {
        uint8_t id = 0;
        uint8_t values_per_entry = 0;
        uint8_t mask = 0;
        uint8_t end_flag = 0;
    };
    struct IndexEntry {
        std::string text;
        std::unordered_map<uint8_t, std::vector<uint32_t>> tags;
    };

    _text_records.clear();
    _decoded_text_length = 0;
    for (uint16_t i = 1; i <= _text_record_count; ++i) {
        std::string decoded;
        if (!decode_text_record(i, &decoded)) {
            _last_error = "Failed to decompress KF8 text record " + std::to_string(i);
            return false;
        }
        _text_records.push_back({i, _decoded_text_length, static_cast<uint32_t>(decoded.size())});
        _decoded_text_length += decoded.size();
    }
    if (_decoded_text_length == 0) {
        _last_error = "KF8 document has no text";
        return false;
    }

    uint64_t main_flow_start = 0;
    uint64_t main_flow_end = _decoded_text_length;
    if (_fdst_record_index != 0xffffffffu && _fdst_flow_count > 1) {
        std::vector<uint8_t> fdst;
        if (!read_record(_fdst_record_index, &fdst) || fdst.size() < 20 ||
            std::memcmp(fdst.data(), "FDST", 4) != 0) {
            _last_error = "Invalid KF8 FDST record";
            return false;
        }
        const uint32_t count = read_be32(fdst.data() + 8);
        if (count == 0 || count > 65536 || 12ULL + static_cast<uint64_t>(count) * 8ULL > fdst.size()) {
            _last_error = "Invalid KF8 FDST flow table";
            return false;
        }
        main_flow_start = read_be32(fdst.data() + 12);
        main_flow_end = read_be32(fdst.data() + 16);
        if (main_flow_start >= main_flow_end || main_flow_end > _decoded_text_length) {
            _last_error = "KF8 main flow is outside decoded text";
            return false;
        }
    }

    auto read_vwi = [](const std::vector<uint8_t> &data, size_t *position,
                       size_t limit, uint32_t *value) -> bool {
        if (!position || !value) return false;
        uint32_t result = 0;
        for (unsigned i = 0; i < 5 && *position < limit; ++i) {
            const uint8_t byte = data[(*position)++];
            if (result > (0xffffffffu >> 7)) return false;
            result = (result << 7) | (byte & 0x7fu);
            if (byte & 0x80u) {
                *value = result;
                return true;
            }
        }
        return false;
    };

    auto parse_index = [&](uint32_t record_index, std::vector<IndexEntry> *entries) -> bool {
        if (!entries || record_index == 0xffffffffu || record_index >= _records.size()) return false;
        entries->clear();
        std::vector<uint8_t> main;
        if (!read_record(record_index, &main) || main.size() < 0x38 ||
            std::memcmp(main.data(), "INDX", 4) != 0) return false;
        const uint32_t header_length = read_be32(main.data() + 4);
        const uint32_t record_count = read_be32(main.data() + 0x18);
        if (header_length > main.size() || record_count == 0 || record_count > 65535 ||
            static_cast<uint64_t>(record_index) + record_count >= _records.size() ||
            header_length + 12 > main.size() ||
            std::memcmp(main.data() + header_length, "TAGX", 4) != 0) return false;
        const uint32_t tagx_length = read_be32(main.data() + header_length + 4);
        const uint32_t control_bytes = read_be32(main.data() + header_length + 8);
        if (tagx_length < 12 || header_length + tagx_length > main.size() ||
            control_bytes == 0 || control_bytes > 64) return false;

        std::vector<TagDefinition> definitions;
        for (size_t pos = header_length + 12; pos + 4 <= header_length + tagx_length; pos += 4) {
            definitions.push_back({main[pos], main[pos + 1], main[pos + 2], main[pos + 3]});
        }

        for (uint32_t record_number = 1; record_number <= record_count; ++record_number) {
            std::vector<uint8_t> data;
            if (!read_record(record_index + record_number, &data) || data.size() < 0x1c ||
                std::memcmp(data.data(), "INDX", 4) != 0) return false;
            const uint32_t idxt = read_be32(data.data() + 0x14);
            const uint32_t entry_count = read_be32(data.data() + 0x18);
            if (idxt > 0xffffu || idxt + 4ULL + entry_count * 2ULL > data.size() ||
                std::memcmp(data.data() + idxt, "IDXT", 4) != 0) return false;
            std::vector<uint16_t> positions;
            positions.reserve(entry_count + 1);
            for (uint32_t i = 0; i < entry_count; ++i) {
                const uint16_t offset = read_be16(data.data() + idxt + 4 + i * 2);
                if (offset >= idxt) return false;
                positions.push_back(offset);
            }
            positions.push_back(static_cast<uint16_t>(std::min<uint32_t>(idxt, 0xffffu)));

            for (uint32_t i = 0; i < entry_count; ++i) {
                size_t pos = positions[i];
                const size_t end = positions[i + 1];
                if (pos >= end || end > idxt) return false;
                const uint8_t text_length = data[pos++];
                if (pos + text_length > end || pos + text_length + control_bytes > end) return false;
                IndexEntry entry;
                entry.text.assign(reinterpret_cast<const char *>(data.data() + pos), text_length);
                pos += text_length;
                const size_t control_start = pos;
                size_t value_pos = control_start + control_bytes;
                size_t control_index = 0;
                struct PendingTag { uint8_t id; uint32_t count; uint32_t bytes; uint8_t values_per; };
                std::vector<PendingTag> pending;
                for (const auto &definition : definitions) {
                    if (definition.end_flag == 1) {
                        ++control_index;
                        continue;
                    }
                    if (control_index >= control_bytes) return false;
                    uint8_t masked = data[control_start + control_index] & definition.mask;
                    if (!masked) continue;
                    if (masked == definition.mask && (definition.mask & (definition.mask - 1u))) {
                        uint32_t byte_count = 0;
                        if (!read_vwi(data, &value_pos, end, &byte_count)) return false;
                        pending.push_back({definition.id, 0, byte_count, definition.values_per_entry});
                    } else {
                        uint8_t mask = definition.mask;
                        while ((mask & 1u) == 0) { mask >>= 1; masked >>= 1; }
                        pending.push_back({definition.id, masked, 0, definition.values_per_entry});
                    }
                }
                for (const auto &item : pending) {
                    auto &values = entry.tags[item.id];
                    if (item.bytes) {
                        const size_t target = value_pos + item.bytes;
                        if (target > end) return false;
                        while (value_pos < target) {
                            uint32_t value = 0;
                            if (!read_vwi(data, &value_pos, target, &value)) return false;
                            values.push_back(value);
                        }
                    } else {
                        const uint32_t value_count = item.count * item.values_per;
                        for (uint32_t n = 0; n < value_count; ++n) {
                            uint32_t value = 0;
                            if (!read_vwi(data, &value_pos, end, &value)) return false;
                            values.push_back(value);
                        }
                    }
                }
                entries->push_back(std::move(entry));
            }
        }
        return !entries->empty();
    };

    std::vector<IndexEntry> skeletons;
    std::vector<IndexEntry> fragments;
    if (!parse_index(_skeleton_index, &skeletons) || !parse_index(_fragment_index, &fragments)) {
        _last_error = "Could not parse KF8 SKEL/FRAG indexes";
        return false;
    }

    _kf8_parts.clear();
    _sections.clear();
    _toc.clear();
    _kf8_parts.reserve(skeletons.size());
    _sections.reserve(skeletons.size());
    size_t fragment_cursor = 0;
    uint64_t assembled_position = 0;
    for (const auto &skeleton : skeletons) {
        const auto count_it = skeleton.tags.find(1);
        const auto range_it = skeleton.tags.find(6);
        if (count_it == skeleton.tags.end() || count_it->second.empty() ||
            range_it == skeleton.tags.end() || range_it->second.size() < 2) {
            _last_error = "Invalid KF8 skeleton entry";
            return false;
        }
        const uint32_t fragment_count = count_it->second[0];
        const uint64_t skeleton_start = range_it->second[0];
        const uint32_t skeleton_length = range_it->second[1];
        if (skeleton_start < main_flow_start || skeleton_start + skeleton_length > main_flow_end ||
            fragment_cursor + fragment_count > fragments.size()) {
            _last_error = "KF8 skeleton range is outside the main flow";
            return false;
        }
        Kf8Part part;
        part.skeleton_start = skeleton_start;
        part.skeleton_length = skeleton_length;
        uint64_t fragment_data = skeleton_start + skeleton_length;
        uint64_t assembled_length = skeleton_length;
        part.fragments.reserve(fragment_count);
        for (uint32_t n = 0; n < fragment_count; ++n, ++fragment_cursor) {
            const IndexEntry &fragment = fragments[fragment_cursor];
            const auto fragment_range = fragment.tags.find(6);
            if (fragment_range == fragment.tags.end() || fragment_range->second.size() < 2) {
                _last_error = "Invalid KF8 fragment entry";
                return false;
            }
            char *end = nullptr;
            const uint64_t absolute_insert = std::strtoull(fragment.text.c_str(), &end, 10);
            const uint32_t length = fragment_range->second[1];
            const uint64_t relative_insert = absolute_insert >= skeleton_start
                                                 ? absolute_insert - skeleton_start
                                                 : UINT64_MAX;
            // Fragment positions are relative to the progressively assembled
            // skeleton. A later fragment may therefore be inserted beyond the
            // original skeleton length, but never beyond the current result.
            if (!end || end == fragment.text.c_str() || *end != '\0' ||
                fragment_data + length > main_flow_end ||
                relative_insert > assembled_length || relative_insert > 0xffffffffu) {
                _last_error = "Invalid KF8 fragment range";
                return false;
            }
            part.fragments.push_back({static_cast<uint32_t>(relative_insert),
                                      fragment_data, length});
            fragment_data += length;
            assembled_length += length;
        }
        if (assembled_length > 0xffffffffu) {
            _last_error = "KF8 section is too large";
            return false;
        }
        part.assembled_length = static_cast<uint32_t>(assembled_length);
        _kf8_parts.push_back(std::move(part));

        MobiSectionItem section;
        section.id = "section-" + std::to_string(_sections.size());
        section.href = section.id + ".html";
        section.start = assembled_position;
        section.end = assembled_position + assembled_length;
        section.uncompressed_size = static_cast<uint32_t>(assembled_length);
        assembled_position = section.end;
        _sections.push_back(std::move(section));
    }
    if (_kf8_parts.empty()) {
        _last_error = "KF8 document has no reconstructed sections";
        return false;
    }

    for (size_t i = 0; i < _kf8_parts.size(); ++i) {
        MobiTocItem item;
        // Reconstructing every section here would decompress most of the book
        // a second time during open. The real heading is available when that
        // section is loaded; keep first-open indexing bounded and predictable.
        item.title = "Section " + std::to_string(i + 1);
        item.href = _sections[i].href;
        item.spine_index = static_cast<int>(i);
        _toc.push_back(std::move(item));
    }
    ESP_LOGI(TAG, "KF8 reconstructed sections=%u fragments=%u raw=%llu assembled=%llu",
             static_cast<unsigned>(_kf8_parts.size()), static_cast<unsigned>(fragments.size()),
             static_cast<unsigned long long>(_decoded_text_length),
             static_cast<unsigned long long>(assembled_position));
    return true;
}

bool MobiBook::read_kf8_part(size_t index, std::string *out) const
{
    if (!out || index >= _kf8_parts.size()) return false;
    const Kf8Part &part = _kf8_parts[index];
    if (!read_text_range(part.skeleton_start, part.skeleton_start + part.skeleton_length, out)) return false;
    out->reserve(part.assembled_length);
    for (const auto &fragment : part.fragments) {
        std::string bytes;
        if (!read_text_range(fragment.raw_start, fragment.raw_start + fragment.length, &bytes)) return false;
        // KF8 insert offsets are applied to the progressively rebuilt
        // skeleton exactly as stored by the fragment table.
        const uint64_t position = fragment.insert_offset;
        if (position > out->size()) return false;
        out->insert(static_cast<size_t>(position), bytes);
    }
    return !out->empty();
}

bool MobiBook::build_text_index()
{
    if (_is_kf8) return build_kf8_index();
    std::vector<uint64_t> boundaries{0};
    uint64_t last_boundary = 0;
    std::string tail;
    for (uint16_t i = 1; i <= _text_record_count; ++i) {
        std::string decoded;
        if (!decode_text_record(i, &decoded)) {
            _last_error = "Failed to decompress MOBI text record " + std::to_string(i);
            return false;
        }
        uint64_t start = _decoded_text_length;
        _text_records.push_back({i, start, static_cast<uint32_t>(decoded.size())});

        std::string scan = tail + decoded;
        std::string lower = lower_ascii(scan);
        size_t pos = 0;
        while ((pos = lower.find("<mbp:pagebreak", pos)) != std::string::npos) {
            uint64_t absolute = start >= tail.size() ? start - tail.size() + pos : pos;
            if (absolute > last_boundary + MIN_SECTION_BYTES) {
                boundaries.push_back(absolute);
                last_boundary = absolute;
            }
            pos += 14;
        }
        if (start > last_boundary + TARGET_SECTION_BYTES) {
            size_t split = 0;
            size_t paragraph_end = lower.find("</p>", tail.size());
            size_t division_end = lower.find("</div>", tail.size());
            if (paragraph_end != std::string::npos) split = paragraph_end + 4 - tail.size();
            else if (division_end != std::string::npos) split = division_end + 6 - tail.size();
            uint64_t boundary = start + std::min(split, decoded.size());
            if (boundary > last_boundary + MIN_SECTION_BYTES) {
                boundaries.push_back(boundary);
                last_boundary = boundary;
            }
        }
        _decoded_text_length += decoded.size();
        tail = scan.size() > 64 ? scan.substr(scan.size() - 64) : scan;
    }
    if (_decoded_text_length == 0) {
        _last_error = "MOBI document has no text";
        return false;
    }
    if (boundaries.back() != _decoded_text_length) boundaries.push_back(_decoded_text_length);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
        if (boundaries[i + 1] <= boundaries[i]) continue;
        MobiSectionItem section;
        section.id = "section-" + std::to_string(_sections.size());
        section.href = section.id + ".html";
        section.start = boundaries[i];
        section.end = boundaries[i + 1];
        section.uncompressed_size = static_cast<uint32_t>(
            std::min<uint64_t>(0xffffffffu, section.end - section.start));
        _sections.push_back(std::move(section));
    }

    for (size_t i = 0; i < _sections.size(); ++i) {
        std::string html;
        std::string label;
        // Chapter labels normally occur at the beginning of a section. Probe
        // only a small prefix instead of decoding the complete book a second
        // time while creating the first metadata cache.
        uint64_t probe_end = std::min<uint64_t>(_sections[i].end, _sections[i].start + 8 * 1024);
        if (read_text_range(_sections[i].start, probe_end, &html)) label = first_heading(html);
        if (label.empty()) label = "Section " + std::to_string(i + 1);
        MobiTocItem item;
        item.title = std::move(label);
        item.href = _sections[i].href;
        item.spine_index = static_cast<int>(i);
        _toc.push_back(std::move(item));
    }
    return !_sections.empty();
}

bool MobiBook::read_text_range(uint64_t start, uint64_t end, std::string *out) const
{
    if (!out || start >= end || end > _decoded_text_length) return false;
    out->clear();
    out->reserve(static_cast<size_t>(std::min<uint64_t>(end - start, TARGET_SECTION_BYTES + 65536)));
    for (const TextRecord &record : _text_records) {
        uint64_t record_end = record.start + record.length;
        if (record_end <= start) continue;
        if (record.start >= end) break;
        std::string decoded;
        if (!decode_text_record(record.record_index, &decoded)) return false;
        uint64_t from = start > record.start ? start - record.start : 0;
        uint64_t to = std::min<uint64_t>(decoded.size(), end - record.start);
        if (from < to) out->append(decoded, static_cast<size_t>(from), static_cast<size_t>(to - from));
    }
    return !out->empty();
}

bool MobiBook::read_section_to_string(const std::string &href, std::string *out) const
{
    const MobiSectionItem *section = nullptr;
    size_t section_index = 0;
    for (size_t i = 0; i < _sections.size(); ++i) {
        const auto &candidate = _sections[i];
        if (candidate.href == href) {
            section = &candidate;
            section_index = i;
            break;
        }
    }
    if (!section) return false;
    if (_is_kf8) {
        if (!read_kf8_part(section_index, out)) return false;
    } else if (!read_text_range(section->start, section->end, out)) {
        return false;
    }

    std::string normalized;
    normalized.reserve(out->size() + 128);
    size_t pos = 0;
    while (pos < out->size()) {
        size_t start = out->find('<', pos);
        if (start == std::string::npos) {
            normalized.append(*out, pos, std::string::npos);
            break;
        }
        normalized.append(*out, pos, start - pos);
        size_t end = out->find('>', start + 1);
        if (end == std::string::npos) {
            normalized.append(*out, start, std::string::npos);
            break;
        }
        std::string tag = out->substr(start + 1, end - start - 1);
        std::string lower = lower_ascii(tag);
        // Some AZW/hybrid generators leave KF8-style image URIs in the
        // MOBI7-facing markup as well, so normalize these based on the URI
        // itself rather than gating the conversion on the header version.
        {
            size_t embed = 0;
            while ((embed = lower.find("kindle:embed:", embed)) != std::string::npos) {
                const size_t digits = embed + 13;
                size_t digit_end = digits;
                uint32_t number = 0;
                while (digit_end < tag.size()) {
                    const int value = base32_digit(static_cast<unsigned char>(tag[digit_end]));
                    if (value < 0 || number > (0xffffffffu - static_cast<uint32_t>(value)) / 32u) break;
                    number = number * 32u + static_cast<uint32_t>(value);
                    ++digit_end;
                }
                if (digit_end == digits || number == 0) {
                    embed = digits;
                    continue;
                }
                size_t uri_end = digit_end;
                if (uri_end < tag.size() && tag[uri_end] == '?') {
                    while (uri_end < tag.size() && tag[uri_end] != '\'' && tag[uri_end] != '"' &&
                           tag[uri_end] != ')' &&
                           !std::isspace(static_cast<unsigned char>(tag[uri_end]))) ++uri_end;
                }
                const std::string replacement = resource_href(number);
                if (replacement.empty()) {
                    embed = digit_end;
                    continue;
                }
                tag.replace(embed, uri_end - embed, replacement);
                lower = lower_ascii(tag);
                embed += replacement.size();
            }
        }
        if (lower.find("mbp:pagebreak") == 0) {
            normalized += "<pagebreak/>";
        } else if (!lower.empty() && lower[0] == 'a' &&
                   (lower.size() == 1 || std::isspace(static_cast<unsigned char>(lower[1]))) &&
                   lower.find("href=") == std::string::npos) {
            uint32_t filepos = 0;
            const MobiSectionItem *target_section = nullptr;
            if (parse_decimal_attr(tag, "filepos", &filepos)) {
                for (const auto &candidate : _sections) {
                    if (filepos >= candidate.start && filepos < candidate.end) {
                        target_section = &candidate;
                        break;
                    }
                }
            }
            if (target_section) {
                while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back()))) tag.pop_back();
                normalized += '<';
                normalized += tag;
                normalized += " href=\"" + target_section->href + "#mobi-filepos-" +
                              std::to_string(filepos) + "\">";
            } else {
                normalized.append(*out, start, end - start + 1);
            }
        } else if (lower.find("img") == 0 && lower.find("src=") == std::string::npos) {
            uint32_t recindex = 0;
            if (parse_decimal_attr(tag, "recindex", &recindex) && recindex > 0) {
                while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back()))) tag.pop_back();
                bool self_closing = !tag.empty() && tag.back() == '/';
                if (self_closing) tag.pop_back();
                normalized += '<';
                normalized += tag;
                normalized += " src=\"" + resource_href(recindex) + "\"";
                normalized += self_closing ? "/>" : ">";
            } else {
                normalized.append(*out, start, end - start + 1);
            }
        } else {
            normalized.append(*out, start, end - start + 1);
        }
        pos = end + 1;
    }
    *out = std::move(normalized);
    return true;
}

bool MobiBook::resolve_filepos_footnote(uint64_t position, std::string *out) const
{
    if (!out || position >= _decoded_text_length) return false;
    const MobiSectionItem *section = nullptr;
    for (const auto &candidate : _sections) {
        if (position >= candidate.start && position < candidate.end) {
            section = &candidate;
            break;
        }
    }
    if (!section) return false;

    std::string snippet;
    const uint64_t end = std::min<uint64_t>(section->end, position + 12000);
    if (!read_text_range(position, end, &snippet) || snippet.empty()) return false;

    std::string lower = lower_ascii(snippet);
    size_t cut = std::string::npos;
    for (const char *closing : {"</p", "</li", "</div", "</aside", "<mbp:pagebreak"}) {
        const size_t found = lower.find(closing);
        if (found != std::string::npos && (cut == std::string::npos || found < cut)) cut = found;
    }
    if (cut != std::string::npos) {
        const size_t close_end = snippet.find('>', cut);
        snippet.resize(close_end == std::string::npos ? cut : close_end + 1);
    } else if (snippet.size() > 6000) {
        snippet.resize(6000);
    }

    std::vector<EpubContentBlock> blocks;
    const std::string wrapped = "<html><body><p>" + snippet + "</p></body></html>";
    if (!epub_parse_xhtml_blocks(wrapped, section->href, &blocks)) return false;
    for (const auto &block : blocks) {
        if (block.type == EpubContentBlockType::Text && !block.text.empty()) {
            *out = block.text;
            return true;
        }
    }
    return false;
}

std::string MobiBook::resource_href(uint32_t resource_index) const
{
    if (_first_image_record == 0xffffffffu || resource_index == 0) return std::string();
    size_t record_index = static_cast<size_t>(_first_image_record) + resource_index - 1;
    std::vector<uint8_t> data;
    std::string ext = ".img";
    if (read_record(record_index, &data)) {
        if (data.size() >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) ext = ".jpg";
        else if (data.size() >= 8 && std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8) == 0) ext = ".png";
        else if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M') ext = ".bmp";
        else if (data.size() >= 6 && std::memcmp(data.data(), "GIF8", 4) == 0) ext = ".gif";
    }
    char name[40];
    std::snprintf(name, sizeof(name), "res/%05lu%s", static_cast<unsigned long>(resource_index), ext.c_str());
    return name;
}

std::string MobiBook::normalize_resource_href(const std::string &href) const
{
    std::string lower = lower_ascii(href);
    const size_t prefix = lower.find("kindle:embed:");
    if (prefix == std::string::npos) return href;

    size_t pos = prefix + 13;
    uint32_t number = 0;
    const size_t digits = pos;
    while (pos < href.size()) {
        const int value = base32_digit(static_cast<unsigned char>(href[pos]));
        if (value < 0 || number > (0xffffffffu - static_cast<uint32_t>(value)) / 32u) break;
        number = number * 32u + static_cast<uint32_t>(value);
        ++pos;
    }
    if (pos == digits || number == 0) return href;
    return resource_href(number);
}

bool MobiBook::extract_resource_to_file(const std::string &href, const std::string &out_path) const
{
    size_t slash = href.find_last_of('/');
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    char *end = nullptr;
    unsigned long resource_index = std::strtoul(href.c_str() + start, &end, 10);
    if (resource_index == 0 || end == href.c_str() + start || _first_image_record == 0xffffffffu) return false;
    size_t record_index = static_cast<size_t>(_first_image_record) + resource_index - 1;
    std::vector<uint8_t> data;
    if (!read_record(record_index, &data)) return false;
    FILE *fp = std::fopen(out_path.c_str(), "wb");
    if (!fp) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), fp) == data.size();
    if (std::fclose(fp) != 0) ok = false;
    if (!ok) std::remove(out_path.c_str());
    return ok;
}
