#include "ui/script_conversion.h"

#include "storage/app_paths.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace script_conversion {
namespace {

constexpr char TAG[] = "SCRIPT_CONVERT";
constexpr char NVS_NAMESPACE[] = "script_mode";
constexpr uint8_t MAGIC[8] = {'M', 'P', 'O', 'C', 'C', '0', '1', 0};
struct Header {
    uint8_t magic[8];
    uint32_t version, direction, count, max_codepoints;
    uint32_t records_bytes, data_bytes, crc32;
};
struct Entry { uint32_t key_offset, value_offset, key_bytes, value_bytes; };
static_assert(sizeof(Header) == 36, "OpenCC header size");
static_assert(sizeof(Entry) == 16, "OpenCC entry size");

std::mutex s_lock;
uint8_t *s_blob = nullptr;
size_t s_blob_size = 0;
Mode s_mode = Mode::Original;
uint32_t s_signature = 0;

uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return ~crc;
}

uint32_t hash_path(const char *path)
{
    uint32_t hash = 2166136261u;
    if (path) for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        hash = (hash ^ *p) * 16777619u;
    }
    return hash;
}

void nvs_key(const char *path, char (&key)[16])
{
    snprintf(key, sizeof(key), "b%08lx", (unsigned long)hash_path(path));
}

size_t utf8_size(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xe0) == 0xc0) return 2;
    if ((lead & 0xf0) == 0xe0) return 3;
    if ((lead & 0xf8) == 0xf0) return 4;
    return 1;
}

const Entry *find(const Header *header, const char *key, size_t length)
{
    const auto *records = reinterpret_cast<const Entry *>(header + 1);
    const char *data = reinterpret_cast<const char *>(records) + header->records_bytes;
    size_t lo = 0, hi = header->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const Entry &entry = records[mid];
        int cmp = memcmp(key, data + entry.key_offset, std::min(length, (size_t)entry.key_bytes));
        if (!cmp) cmp = length < entry.key_bytes ? -1 : (length > entry.key_bytes ? 1 : 0);
        if (cmp == 0) return &entry;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return nullptr;
}

} // namespace

const char *mode_name(Mode mode)
{
    switch (mode) {
    case Mode::Simplified: return "Simplified";
    case Mode::Traditional: return "Traditional";
    default: return "Original";
    }
}

Mode book_mode(const char *path)
{
    if (!path || !*path) return Mode::Original;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return Mode::Original;
    char key[16];
    nvs_key(path, key);
    uint8_t value = 0;
    nvs_get_u8(handle, key, &value);
    nvs_close(handle);
    return value <= 2 ? static_cast<Mode>(value) : Mode::Original;
}

bool set_book_mode(const char *path, Mode mode)
{
    if (!path || !*path || static_cast<uint8_t>(mode) > 2) return false;
    if (!prepare(mode)) return false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    char key[16];
    nvs_key(path, key);
    bool ok = nvs_set_u8(handle, key, static_cast<uint8_t>(mode)) == ESP_OK &&
              nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool prepare(Mode mode)
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (mode == Mode::Original) return true;
    if (mode == s_mode && s_blob) return true;
    const char *file = mode == Mode::Simplified ? "t2s.bin" : "s2t.bin";
    std::string path = std::string(app_paths::system_dir()) + "/opencc/" + file;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) {
        ESP_LOGW(TAG, "Dictionary missing: %s", path.c_str());
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    rewind(fp);
    if (length < static_cast<long>(sizeof(Header)) || length > 16 * 1024 * 1024) {
        fclose(fp);
        return false;
    }
    auto *blob = static_cast<uint8_t *>(heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!blob) { fclose(fp); return false; }
    bool ok = fread(blob, 1, (size_t)length, fp) == (size_t)length;
    fclose(fp);
    const auto *header = reinterpret_cast<const Header *>(blob);
    const uint32_t direction = mode == Mode::Traditional ? 1 : 2;
    ok = ok && memcmp(header->magic, MAGIC, sizeof(MAGIC)) == 0 &&
         header->version == 1 && header->direction == direction &&
         header->count > 0 && header->max_codepoints > 0 &&
         header->records_bytes == header->count * sizeof(Entry) &&
         static_cast<uint64_t>(sizeof(Header)) + header->records_bytes + header->data_bytes == (size_t)length &&
         crc32(blob + sizeof(Header), (size_t)length - sizeof(Header)) == header->crc32;
    if (ok) {
        const auto *records = reinterpret_cast<const Entry *>(blob + sizeof(Header));
        for (uint32_t i = 0; i < header->count; ++i) {
            if (records[i].key_bytes == 0 ||
                (uint64_t)records[i].key_offset + records[i].key_bytes > header->data_bytes ||
                (uint64_t)records[i].value_offset + records[i].value_bytes > header->data_bytes) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        ESP_LOGE(TAG, "Invalid dictionary: %s", path.c_str());
        heap_caps_free(blob);
        return false;
    }
    heap_caps_free(s_blob);
    s_blob = blob;
    s_blob_size = (size_t)length;
    s_mode = mode;
    s_signature = header->crc32;
    ESP_LOGI(TAG, "%s loaded: entries=%lu bytes=%u PSRAM", file,
             (unsigned long)header->count, (unsigned)s_blob_size);
    return true;
}

uint32_t dictionary_signature()
{
    std::lock_guard<std::mutex> guard(s_lock);
    return s_signature;
}

size_t Result::source_offset(size_t offset) const
{
    if (points.empty()) return std::min(offset, text.size());
    auto it = std::upper_bound(points.begin(), points.end(), offset,
        [](size_t value, const Point &point) { return value < point.display; });
    if (it == points.begin()) return 0;
    --it;
    return it->source;
}

size_t Result::display_offset(size_t offset) const
{
    if (points.empty()) return std::min(offset, text.size());
    auto it = std::lower_bound(points.begin(), points.end(), offset,
        [](const Point &point, size_t value) { return point.source < value; });
    return it == points.end() ? text.size() : it->display;
}

Result convert(const std::string &source, Mode mode)
{
    Result result;
    if (mode == Mode::Original || !prepare(mode)) {
        result.text = source;
        return result;
    }
    std::lock_guard<std::mutex> guard(s_lock);
    if (!s_blob || s_mode != mode) {
        result.text = source;
        return result;
    }
    const auto *header = reinterpret_cast<const Header *>(s_blob);
    const char *data = reinterpret_cast<const char *>(s_blob + sizeof(Header) + header->records_bytes);
    result.text.reserve(source.size());
    result.points.reserve(source.size() / 3 + 2);
    for (size_t pos = 0; pos < source.size();) {
        if ((unsigned char)source[pos] < 0x80) {
            result.points.push_back({result.text.size(), pos});
            result.text.push_back(source[pos++]);
            continue;
        }
        size_t end = pos;
        size_t count = 0;
        const Entry *match = nullptr;
        size_t match_end = 0;
        while (end < source.size() && count < header->max_codepoints) {
            end = std::min(source.size(), end + utf8_size((unsigned char)source[end]));
            ++count;
            if (const Entry *candidate = find(header, source.data() + pos, end - pos)) {
                match = candidate;
                match_end = end;
            }
        }
        result.points.push_back({result.text.size(), pos});
        if (match) {
            result.text.append(data + match->value_offset, match->value_bytes);
            pos = match_end;
        } else {
            end = std::min(source.size(), pos + utf8_size((unsigned char)source[pos]));
            result.text.append(source, pos, end - pos);
            pos = end;
        }
    }
    result.points.push_back({result.text.size(), source.size()});
    return result;
}

} // namespace script_conversion
