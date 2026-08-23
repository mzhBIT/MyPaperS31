#include "audio/embedded_lyrics.h"
#include "parser/gbk_decode.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <strings.h>

namespace embedded_lyrics {
namespace {

constexpr const char *TAG = "LYRICS";
constexpr size_t MAX_LYRIC_BYTES = 256 * 1024;
constexpr size_t MAX_COMMENT_BYTES = 1024 * 1024;
constexpr size_t MAX_LINES = 2048;
constexpr int MAX_MP4_DEPTH = 8;

using FilePtr = std::unique_ptr<FILE, int (*)(FILE *)>;

uint32_t be32(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint32_t synchsafe32(const uint8_t *p)
{
    return (uint32_t(p[0] & 0x7f) << 21) | (uint32_t(p[1] & 0x7f) << 14) |
           (uint32_t(p[2] & 0x7f) << 7) | uint32_t(p[3] & 0x7f);
}

bool read_exact(FILE *file, void *data, size_t size)
{
    return size == 0 || fread(data, 1, size, file) == size;
}

bool seek_abs(FILE *file, uint64_t offset)
{
    return offset <= LONG_MAX && fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
}

std::string trim(std::string value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

bool equals_ci(const std::string &a, const char *b)
{
    return b != nullptr && a.size() == strlen(b) && strcasecmp(a.c_str(), b) == 0;
}

bool lyric_key(const std::string &key)
{
    return equals_ci(key, "LYRICS") || equals_ci(key, "SYNCEDLYRICS") ||
           equals_ci(key, "UNSYNCEDLYRICS") || equals_ci(key, "SYNCED LYRICS") ||
           equals_ci(key, "LYRICS_SYNC");
}

void append_utf8(std::string &out, uint32_t cp)
{
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::string decode_text(uint8_t encoding, const uint8_t *data, size_t size)
{
    std::string out;
    if (data == nullptr || size == 0) return out;

    if (encoding == 3) {
        out.assign(reinterpret_cast<const char *>(data), size);
        if (out.size() >= 3 && static_cast<uint8_t>(out[0]) == 0xef &&
            static_cast<uint8_t>(out[1]) == 0xbb && static_cast<uint8_t>(out[2]) == 0xbf) {
            out.erase(0, 3);
        }
        return trim(out);
    }
    if (encoding == 0) {
        out.reserve(size * 2);
        for (size_t i = 0; i < size; ++i) append_utf8(out, data[i]);
        return trim(out);
    }

    bool little = false;
    size_t pos = 0;
    if (encoding == 1 && size >= 2) {
        if (data[0] == 0xff && data[1] == 0xfe) {
            little = true;
            pos = 2;
        } else if (data[0] == 0xfe && data[1] == 0xff) {
            pos = 2;
        }
    }
    out.reserve(size);
    while (pos + 1 < size) {
        uint16_t first = little ? uint16_t(data[pos] | (data[pos + 1] << 8))
                                : uint16_t((data[pos] << 8) | data[pos + 1]);
        pos += 2;
        if (first == 0) continue;
        uint32_t cp = first;
        if (first >= 0xd800 && first <= 0xdbff && pos + 1 < size) {
            uint16_t second = little ? uint16_t(data[pos] | (data[pos + 1] << 8))
                                     : uint16_t((data[pos] << 8) | data[pos + 1]);
            if (second >= 0xdc00 && second <= 0xdfff) {
                cp = 0x10000u + ((first - 0xd800u) << 10) + (second - 0xdc00u);
                pos += 2;
            }
        }
        append_utf8(out, cp);
    }
    return trim(out);
}

size_t text_end(uint8_t encoding, const uint8_t *data, size_t size, size_t start)
{
    if (encoding == 0 || encoding == 3) {
        for (size_t i = start; i < size; ++i) {
            if (data[i] == 0) return i;
        }
        return size;
    }
    for (size_t i = start; i + 1 < size; i += 2) {
        if (data[i] == 0 && data[i + 1] == 0) return i;
    }
    return size;
}

size_t terminator_size(uint8_t encoding)
{
    return (encoding == 0 || encoding == 3) ? 1 : 2;
}

bool parse_time_tag(const std::string &tag, uint32_t *time_ms)
{
    size_t colon = tag.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= tag.size()) return false;
    uint64_t minutes = 0;
    for (size_t i = 0; i < colon; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(tag[i]))) return false;
        minutes = minutes * 10 + uint64_t(tag[i] - '0');
        if (minutes > 10000) return false;
    }
    size_t dot = tag.find_first_of(".,", colon + 1);
    size_t second_end = dot == std::string::npos ? tag.size() : dot;
    if (second_end == colon + 1 || second_end - colon - 1 > 2) return false;
    uint32_t seconds = 0;
    for (size_t i = colon + 1; i < second_end; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(tag[i]))) return false;
        seconds = seconds * 10 + uint32_t(tag[i] - '0');
    }
    if (seconds >= 60) return false;
    uint32_t fraction = 0;
    if (dot != std::string::npos) {
        size_t digits = 0;
        for (size_t i = dot + 1; i < tag.size() && digits < 3; ++i, ++digits) {
            if (!std::isdigit(static_cast<unsigned char>(tag[i]))) return false;
            fraction = fraction * 10 + uint32_t(tag[i] - '0');
        }
        if (digits == 1) fraction *= 100;
        else if (digits == 2) fraction *= 10;
    }
    uint64_t result = (minutes * 60 + seconds) * 1000 + fraction;
    if (result > UINT32_MAX) return false;
    *time_ms = static_cast<uint32_t>(result);
    return true;
}

bool parse_lrc(const std::string &source, std::vector<Line> &lines)
{
    if (source.empty()) return false;
    size_t before = lines.size();
    int32_t offset_ms = 0;
    size_t pos = 0;
    while (pos < source.size() && lines.size() < MAX_LINES) {
        size_t end = source.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = source.size();
        std::string row = source.substr(pos, end - pos);
        pos = end;
        while (pos < source.size() && (source[pos] == '\r' || source[pos] == '\n')) ++pos;

        std::vector<uint32_t> timestamps;
        size_t cursor = 0;
        while (cursor < row.size() && row[cursor] == '[') {
            size_t close = row.find(']', cursor + 1);
            if (close == std::string::npos) break;
            std::string tag = row.substr(cursor + 1, close - cursor - 1);
            uint32_t timestamp = 0;
            if (parse_time_tag(tag, &timestamp)) {
                timestamps.push_back(timestamp);
            } else if (tag.size() > 7 && strncasecmp(tag.c_str(), "offset:", 7) == 0) {
                const char *number = tag.c_str() + 7;
                char *tail = nullptr;
                long parsed = strtol(number, &tail, 10);
                if (tail != number && *tail == '\0') {
                    offset_ms = static_cast<int32_t>(std::max<long>(-600000, std::min<long>(600000, parsed)));
                }
            }
            cursor = close + 1;
        }
        std::string text = trim(row.substr(cursor));
        if (text.empty()) continue;
        for (uint32_t timestamp : timestamps) {
            if (lines.size() >= MAX_LINES) break;
            lines.push_back({timestamp, text});
        }
    }

    if (offset_ms != 0) {
        for (size_t i = before; i < lines.size(); ++i) {
            int64_t shifted = int64_t(lines[i].time_ms) + offset_ms;
            lines[i].time_ms = static_cast<uint32_t>(std::max<int64_t>(0, std::min<int64_t>(UINT32_MAX, shifted)));
        }
    }
    return lines.size() > before;
}

bool valid_utf8(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size;) {
        uint8_t first = data[i];
        if (first < 0x80) {
            ++i;
            continue;
        }
        size_t length = 0;
        if ((first & 0xe0) == 0xc0) length = 2;
        else if ((first & 0xf0) == 0xe0) length = 3;
        else if ((first & 0xf8) == 0xf0) length = 4;
        else return false;
        if (i + length > size) return false;
        for (size_t j = 1; j < length; ++j) {
            if ((data[i + j] & 0xc0) != 0x80) return false;
        }
        i += length;
    }
    return true;
}

bool likely_gbk(const uint8_t *data, size_t size)
{
    size_t pairs = 0;
    size_t invalid = 0;
    size = std::min(size, size_t(4096));
    for (size_t i = 0; i < size;) {
        if (data[i] < 0x80) {
            ++i;
        } else if (data[i] >= 0x81 && data[i] <= 0xfe && i + 1 < size &&
                   data[i + 1] >= 0x40 && data[i + 1] <= 0xfe && data[i + 1] != 0x7f) {
            ++pairs;
            i += 2;
        } else {
            ++invalid;
            ++i;
        }
    }
    return pairs > 0 && invalid <= pairs / 10 + 2;
}

std::string decode_lrc_file(const uint8_t *data, size_t size)
{
    if (data == nullptr || size == 0) return {};
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        return std::string(reinterpret_cast<const char *>(data + 3), size - 3);
    }
    if (size >= 2 && ((data[0] == 0xff && data[1] == 0xfe) ||
                      (data[0] == 0xfe && data[1] == 0xff))) {
        return decode_text(1, data, size);
    }
    if (valid_utf8(data, size)) {
        return std::string(reinterpret_cast<const char *>(data), size);
    }
    if (likely_gbk(data, size)) {
        std::string out;
        out.reserve(size * 2);
        for (size_t i = 0; i < size;) {
            if (data[i] < 0x80) {
                out.push_back(static_cast<char>(data[i++]));
            } else if (i + 1 < size && data[i] >= 0x81 && data[i] <= 0xfe &&
                       data[i + 1] >= 0x40 && data[i + 1] <= 0xfe && data[i + 1] != 0x7f) {
                append_utf8(out, gbk_decode_codepoint(data[i], data[i + 1]));
                i += 2;
            } else {
                append_utf8(out, 0xfffd);
                ++i;
            }
        }
        return out;
    }
    return decode_text(0, data, size);
}

bool load_lrc_path(const std::string &path, std::vector<Line> &lines)
{
    FilePtr file(fopen(path.c_str(), "rb"), fclose);
    if (!file || fseek(file.get(), 0, SEEK_END) != 0) return false;
    long length = ftell(file.get());
    if (length <= 0 || static_cast<size_t>(length) > MAX_LYRIC_BYTES ||
        fseek(file.get(), 0, SEEK_SET) != 0) {
        if (length > static_cast<long>(MAX_LYRIC_BYTES)) {
            ESP_LOGW(TAG, "External LRC exceeds %u bytes: %s",
                     (unsigned)MAX_LYRIC_BYTES, path.c_str());
        }
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if (!read_exact(file.get(), bytes.data(), bytes.size())) return false;
    return parse_lrc(decode_lrc_file(bytes.data(), bytes.size()), lines);
}

bool load_external_lrc(const char *audio_path, std::vector<Line> &lines,
                       std::string *matched_path)
{
    std::string stem(audio_path);
    size_t slash = stem.find_last_of("/\\");
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) stem.erase(dot);

    const char *extensions[] = {".lrc", ".LRC"};
    for (const char *extension : extensions) {
        std::string candidate = stem + extension;
        if (load_lrc_path(candidate, lines)) {
            if (matched_path != nullptr) *matched_path = candidate;
            return true;
        }
        lines.clear();
    }
    return false;
}

void remove_unsynchronization(std::vector<uint8_t> &data)
{
    size_t out = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (i > 0 && data[i] == 0x00 && data[i - 1] == 0xff) continue;
        data[out++] = data[i];
    }
    data.resize(out);
}

void parse_uslt(const std::vector<uint8_t> &payload, std::vector<Line> &lines)
{
    if (payload.size() < 5) return;
    uint8_t encoding = payload[0];
    size_t desc = 4;
    size_t end = text_end(encoding, payload.data(), payload.size(), desc);
    size_t lyric = std::min(payload.size(), end + terminator_size(encoding));
    if (lyric >= payload.size()) return;
    parse_lrc(decode_text(encoding, payload.data() + lyric, payload.size() - lyric), lines);
}

void parse_txxx(const std::vector<uint8_t> &payload, std::vector<Line> &lines)
{
    if (payload.size() < 3) return;
    uint8_t encoding = payload[0];
    size_t end = text_end(encoding, payload.data(), payload.size(), 1);
    std::string description = decode_text(encoding, payload.data() + 1, end - 1);
    size_t value = std::min(payload.size(), end + terminator_size(encoding));
    if (value < payload.size() && lyric_key(description)) {
        parse_lrc(decode_text(encoding, payload.data() + value, payload.size() - value), lines);
    }
}

void parse_sylt(const std::vector<uint8_t> &payload, std::vector<Line> &lines)
{
    if (payload.size() < 8 || payload[4] != 2) return;  // Only millisecond timestamps.
    uint8_t encoding = payload[0];
    size_t pos = 6;
    size_t end = text_end(encoding, payload.data(), payload.size(), pos);
    pos = std::min(payload.size(), end + terminator_size(encoding));
    while (pos < payload.size() && lines.size() < MAX_LINES) {
        end = text_end(encoding, payload.data(), payload.size(), pos);
        if (end >= payload.size() || end + terminator_size(encoding) + 4 > payload.size()) break;
        std::string text = decode_text(encoding, payload.data() + pos, end - pos);
        pos = end + terminator_size(encoding);
        uint32_t timestamp = be32(payload.data() + pos);
        pos += 4;
        if (!text.empty()) lines.push_back({timestamp, text});
    }
}

bool parse_id3(FILE *file, uint64_t offset, std::vector<Line> &lines)
{
    if (!seek_abs(file, offset)) return false;
    uint8_t header[10] = {};
    if (!read_exact(file, header, sizeof(header)) || memcmp(header, "ID3", 3) != 0) return false;
    int version = header[3];
    if (version < 2 || version > 4) return false;
    uint64_t tag_end = offset + 10u + synchsafe32(header + 6);
    bool tag_unsync = (header[5] & 0x80) != 0;

    uint64_t cursor = offset + 10;
    if ((header[5] & 0x40) != 0 && version >= 3) {
        uint8_t size_bytes[4] = {};
        if (!seek_abs(file, cursor) || !read_exact(file, size_bytes, 4)) return false;
        uint32_t ext_size = version == 4 ? synchsafe32(size_bytes) : be32(size_bytes);
        cursor += version == 4 ? ext_size : uint64_t(ext_size) + 4;
    }

    while (cursor + (version == 2 ? 6u : 10u) <= tag_end && lines.size() < MAX_LINES) {
        if (!seek_abs(file, cursor)) break;
        uint8_t frame[10] = {};
        size_t header_size = version == 2 ? 6 : 10;
        if (!read_exact(file, frame, header_size) || frame[0] == 0) break;
        char id[5] = {};
        memcpy(id, frame, version == 2 ? 3 : 4);
        uint32_t size = version == 2
                            ? (uint32_t(frame[3]) << 16) | (uint32_t(frame[4]) << 8) | frame[5]
                            : (version == 4 ? synchsafe32(frame + 4) : be32(frame + 4));
        uint64_t payload_at = cursor + header_size;
        cursor = payload_at + size;
        if (size == 0 || cursor > tag_end) break;

        bool wanted = strcmp(id, "USLT") == 0 || strcmp(id, "SYLT") == 0 ||
                      strcmp(id, "TXXX") == 0 || strcmp(id, "ULT") == 0 ||
                      strcmp(id, "SLT") == 0 || strcmp(id, "TXX") == 0;
        if (!wanted || size > MAX_LYRIC_BYTES) continue;
        if (version == 3 && (frame[9] & 0xe0) != 0) continue;
        if (version == 4 && (frame[9] & 0x4d) != 0) continue;

        std::vector<uint8_t> payload(size);
        if (!seek_abs(file, payload_at) || !read_exact(file, payload.data(), payload.size())) break;
        if (tag_unsync || (version == 4 && (frame[9] & 0x02) != 0)) remove_unsynchronization(payload);
        if (strcmp(id, "USLT") == 0 || strcmp(id, "ULT") == 0) parse_uslt(payload, lines);
        else if (strcmp(id, "SYLT") == 0 || strcmp(id, "SLT") == 0) parse_sylt(payload, lines);
        else parse_txxx(payload, lines);
    }
    return true;
}

bool parse_vorbis_comments(const uint8_t *data, size_t size, std::vector<Line> &lines)
{
    if (size < 8) return false;
    size_t pos = 0;
    uint32_t vendor = le32(data + pos);
    pos += 4;
    if (vendor > size - pos) return false;
    pos += vendor;
    if (pos + 4 > size) return false;
    uint32_t count = le32(data + pos);
    pos += 4;
    bool found = false;
    for (uint32_t i = 0; i < count && pos + 4 <= size; ++i) {
        uint32_t length = le32(data + pos);
        pos += 4;
        if (length > size - pos) break;
        const char *text = reinterpret_cast<const char *>(data + pos);
        const char *equals = static_cast<const char *>(memchr(text, '=', length));
        if (equals != nullptr) {
            std::string key(text, equals - text);
            if (lyric_key(key)) {
                std::string value(equals + 1, text + length);
                found |= parse_lrc(value, lines);
            }
        }
        pos += length;
    }
    return found;
}

void parse_flac(FILE *file, std::vector<Line> &lines)
{
    if (!seek_abs(file, 0)) return;
    uint8_t first[10] = {};
    if (!read_exact(file, first, sizeof(first))) return;
    uint64_t offset = 0;
    if (memcmp(first, "ID3", 3) == 0) {
        offset = 10u + synchsafe32(first + 6);
    }
    if (!seek_abs(file, offset)) return;
    uint8_t marker[4] = {};
    if (!read_exact(file, marker, sizeof(marker)) || memcmp(marker, "fLaC", 4) != 0) return;
    bool last = false;
    while (!last) {
        uint8_t header[4] = {};
        if (!read_exact(file, header, sizeof(header))) break;
        last = (header[0] & 0x80) != 0;
        uint8_t type = header[0] & 0x7f;
        uint32_t size = (uint32_t(header[1]) << 16) | (uint32_t(header[2]) << 8) | header[3];
        if (type == 4 && size <= MAX_COMMENT_BYTES) {
            std::vector<uint8_t> comments(size);
            if (!read_exact(file, comments.data(), comments.size())) break;
            parse_vorbis_comments(comments.data(), comments.size(), lines);
        } else if (fseek(file, static_cast<long>(size), SEEK_CUR) != 0) {
            break;
        }
    }
}

void parse_ogg(FILE *file, std::vector<Line> &lines)
{
    if (!seek_abs(file, 0)) return;
    std::vector<uint8_t> packet;
    bool dropping = false;
    unsigned packet_number = 0;
    while (packet_number < 4) {
        uint8_t header[27] = {};
        if (!read_exact(file, header, sizeof(header)) || memcmp(header, "OggS", 4) != 0) break;
        uint8_t segment_count = header[26];
        std::vector<uint8_t> lacing(segment_count);
        if (!read_exact(file, lacing.data(), lacing.size())) break;
        for (uint8_t segment_size : lacing) {
            if (dropping || packet.size() + segment_size > MAX_COMMENT_BYTES) {
                if (fseek(file, segment_size, SEEK_CUR) != 0) return;
                dropping = true;
            } else {
                size_t old = packet.size();
                packet.resize(old + segment_size);
                if (!read_exact(file, packet.data() + old, segment_size)) return;
            }
            if (segment_size < 255) {
                ++packet_number;
                if (!dropping) {
                    if (packet.size() >= 8 && memcmp(packet.data(), "OpusTags", 8) == 0) {
                        parse_vorbis_comments(packet.data() + 8, packet.size() - 8, lines);
                        return;
                    }
                    if (packet.size() >= 7 && packet[0] == 3 &&
                        memcmp(packet.data() + 1, "vorbis", 6) == 0) {
                        parse_vorbis_comments(packet.data() + 7, packet.size() - 7, lines);
                        return;
                    }
                }
                packet.clear();
                dropping = false;
            }
        }
    }
}

uint32_t fourcc(const char value[4])
{
    return (uint32_t(uint8_t(value[0])) << 24) | (uint32_t(uint8_t(value[1])) << 16) |
           (uint32_t(uint8_t(value[2])) << 8) | uint8_t(value[3]);
}

bool mp4_container(uint32_t type)
{
    return type == fourcc("moov") || type == fourcc("udta") || type == fourcc("meta") ||
           type == fourcc("ilst");
}

void parse_mp4_atoms(FILE *file, uint64_t start, uint64_t end, int depth,
                     bool inside_lyrics, std::vector<Line> &lines)
{
    if (depth > MAX_MP4_DEPTH || start >= end || lines.size() >= MAX_LINES) return;
    uint64_t pos = start;
    while (pos + 8 <= end) {
        uint8_t header[16] = {};
        if (!seek_abs(file, pos) || !read_exact(file, header, 8)) return;
        uint64_t size = be32(header);
        uint32_t type = be32(header + 4);
        uint64_t header_size = 8;
        if (size == 1) {
            if (!read_exact(file, header + 8, 8)) return;
            size = (uint64_t(be32(header + 8)) << 32) | be32(header + 12);
            header_size = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < header_size || pos + size > end) return;
        uint64_t payload = pos + header_size;
        uint64_t payload_size = size - header_size;
        bool lyric_atom = type == 0xa96c7972u;  // copyright-symbol + "lyr"

        if (inside_lyrics && type == fourcc("data") && payload_size > 8 &&
            payload_size - 8 <= MAX_LYRIC_BYTES) {
            std::vector<uint8_t> text(static_cast<size_t>(payload_size - 8));
            if (seek_abs(file, payload + 8) && read_exact(file, text.data(), text.size())) {
                parse_lrc(std::string(reinterpret_cast<char *>(text.data()), text.size()), lines);
            }
        } else if (lyric_atom || mp4_container(type)) {
            uint64_t child = payload;
            if (type == fourcc("meta") && payload_size >= 4) child += 4;
            parse_mp4_atoms(file, child, pos + size, depth + 1, lyric_atom, lines);
        }
        pos += size;
    }
}

void parse_mp4(FILE *file, std::vector<Line> &lines)
{
    if (fseek(file, 0, SEEK_END) != 0) return;
    long length = ftell(file);
    if (length <= 0) return;
    parse_mp4_atoms(file, 0, static_cast<uint64_t>(length), 0, false, lines);
}

void parse_wave_id3(FILE *file, std::vector<Line> &lines)
{
    if (!seek_abs(file, 0)) return;
    uint8_t header[12] = {};
    if (!read_exact(file, header, sizeof(header)) || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) return;
    uint64_t pos = 12;
    uint64_t end = uint64_t(le32(header + 4)) + 8;
    while (pos + 8 <= end) {
        uint8_t chunk[8] = {};
        if (!seek_abs(file, pos) || !read_exact(file, chunk, sizeof(chunk))) return;
        uint32_t size = le32(chunk + 4);
        if (memcmp(chunk, "ID3 ", 4) == 0 || memcmp(chunk, "id3 ", 4) == 0) {
            parse_id3(file, pos + 8, lines);
            return;
        }
        pos += 8u + size + (size & 1u);
    }
}

void finalize(std::vector<Line> &lines)
{
    std::stable_sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) {
        return a.time_ms < b.time_ms;
    });
    lines.erase(std::unique(lines.begin(), lines.end(), [](const Line &a, const Line &b) {
                    return a.time_ms == b.time_ms && a.text == b.text;
                }),
                lines.end());
    if (lines.size() > MAX_LINES) lines.resize(MAX_LINES);
}

}  // namespace

bool load(const char *path, std::vector<Line> *lines)
{
    if (lines == nullptr) return false;
    lines->clear();
    if (path == nullptr || path[0] == '\0') return false;
    FilePtr file(fopen(path, "rb"), fclose);
    if (!file) return false;

    // An ID3v2 tag may prefix MP3, AAC, FLAC, and a few other containers.
    parse_id3(file.get(), 0, *lines);
    const char *extension = strrchr(path, '.');
    if (extension != nullptr) {
        if (strcasecmp(extension, ".flac") == 0) parse_flac(file.get(), *lines);
        else if (strcasecmp(extension, ".ogg") == 0 || strcasecmp(extension, ".opus") == 0) {
            parse_ogg(file.get(), *lines);
        } else if (strcasecmp(extension, ".m4a") == 0) {
            parse_mp4(file.get(), *lines);
        } else if (strcasecmp(extension, ".wav") == 0) {
            parse_wave_id3(file.get(), *lines);
        }
    }
    finalize(*lines);
    if (!lines->empty()) {
        ESP_LOGI(TAG, "Loaded %u embedded synchronized lines: %s",
                 (unsigned)lines->size(), path);
        return true;
    }

    // Embedded lyrics take precedence. Only fall back to a same-name LRC
    // beside the audio file when no usable embedded timestamps were found.
    file.reset();
    std::string lrc_path;
    if (load_external_lrc(path, *lines, &lrc_path)) {
        finalize(*lines);
        ESP_LOGI(TAG, "Loaded %u external synchronized lines: %s",
                 (unsigned)lines->size(), lrc_path.c_str());
        return !lines->empty();
    }
    return false;
}

}  // namespace embedded_lyrics
