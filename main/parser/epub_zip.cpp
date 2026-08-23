#include "parser/epub_zip.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include <vector>

#include <lgfx/utility/lgfx_miniz.h>

#include "storage/app_paths.h"

static const char *TAG = "EPUB_ZIP";
static const uint32_t ZIP_INDEX_MAGIC = 0x58495A45; // EZIX
static const uint32_t ZIP_INDEX_VERSION = 1;
static const uint32_t ZIP_INDEX_MAX_BYTES = 8 * 1024 * 1024;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void append16(std::vector<uint8_t> *out, uint16_t value)
{
    out->push_back((uint8_t)value);
    out->push_back((uint8_t)(value >> 8));
}

static void append32(std::vector<uint8_t> *out, uint32_t value)
{
    append16(out, (uint16_t)value);
    append16(out, (uint16_t)(value >> 16));
}

static void append64(std::vector<uint8_t> *out, uint64_t value)
{
    append32(out, (uint32_t)value);
    append32(out, (uint32_t)(value >> 32));
}

static uint64_t path_hash64(const char *path)
{
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t *p = (const uint8_t *)(path ? path : ""); *p; ++p) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string EpubZip::normalize_path(const std::string &path)
{
    std::string out = path;
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && out[0] == '/') out.erase(out.begin());
    return out;
}

bool EpubZip::open(const char *path)
{
    close();
    if (!path || !path[0]) return false;

    int64_t t0 = esp_timer_get_time();
    _path = path;
    if (load_index_cache(path)) {
        ESP_LOGI(TAG, "opened cached entries=%u time=%lld ms path=%s",
                 (unsigned)_entries.size(),
                 (esp_timer_get_time() - t0) / 1000,
                 path);
        return true;
    }

    FILE *fp = std::fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return false;
    }

    bool ok = load_central_directory(fp);
    std::fclose(fp);
    if (!ok) {
        ESP_LOGE(TAG, "central directory load failed: %s", path);
        close();
        return false;
    }

    save_index_cache(path);
    ESP_LOGI(TAG, "opened entries=%u time=%lld ms path=%s",
             (unsigned)_entries.size(),
             (esp_timer_get_time() - t0) / 1000,
             path);
    return true;
}

void EpubZip::close()
{
    _path.clear();
    _entries.clear();
}

bool EpubZip::has_entry(const std::string &name) const
{
    return _entries.find(normalize_path(name)) != _entries.end();
}

bool EpubZip::entry_info(const std::string &name, Entry *entry) const
{
    auto it = _entries.find(normalize_path(name));
    if (it == _entries.end()) return false;
    if (entry) *entry = it->second;
    return true;
}

bool EpubZip::load_central_directory(FILE *fp)
{
    if (std::fseek(fp, 0, SEEK_END) != 0) return false;
    long file_size = std::ftell(fp);
    if (file_size < 22) return false;

    const long scan_size = std::min<long>(file_size, 65557);
    std::vector<uint8_t> tail((size_t)scan_size);
    if (std::fseek(fp, file_size - scan_size, SEEK_SET) != 0) return false;
    if (std::fread(tail.data(), 1, tail.size(), fp) != tail.size()) return false;

    long eocd = -1;
    for (long i = scan_size - 22; i >= 0; --i) {
        if (tail[(size_t)i] == 0x50 && tail[(size_t)i + 1] == 0x4b &&
            tail[(size_t)i + 2] == 0x05 && tail[(size_t)i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return false;

    const uint8_t *p = tail.data() + eocd;
    uint16_t entry_count = rd16(p + 10);
    uint32_t central_size = rd32(p + 12);
    uint32_t central_offset = rd32(p + 16);
    if (central_offset == 0 || central_size == 0) return false;

    if (std::fseek(fp, (long)central_offset, SEEK_SET) != 0) return false;
    if (central_size > ZIP_INDEX_MAX_BYTES) {
        ESP_LOGW(TAG, "central directory too large for bulk read, using streaming parse: %u",
                 (unsigned)central_size);
        _entries.reserve(entry_count);
        for (uint16_t i = 0; i < entry_count; ++i) {
            uint8_t hdr[46];
            if (std::fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) || rd32(hdr) != 0x02014b50) return false;
            Entry entry;
            entry.method = rd16(hdr + 10);
            entry.compressed_size = rd32(hdr + 20);
            entry.uncompressed_size = rd32(hdr + 24);
            uint16_t name_len = rd16(hdr + 28);
            uint16_t extra_len = rd16(hdr + 30);
            uint16_t comment_len = rd16(hdr + 32);
            entry.local_header_offset = rd32(hdr + 42);
            std::string name(name_len, '\0');
            if (name_len > 0 && std::fread(&name[0], 1, name_len, fp) != name_len) return false;
            if (std::fseek(fp, (long)extra_len + comment_len, SEEK_CUR) != 0) return false;
            name = normalize_path(name);
            if (!name.empty() && name.back() != '/') _entries[name] = entry;
        }
        return true;
    }
    std::vector<uint8_t> central((size_t)central_size);
    if (!central.empty() && std::fread(central.data(), 1, central.size(), fp) != central.size()) return false;
    _entries.reserve(entry_count);

    size_t pos = 0;
    for (uint16_t i = 0; i < entry_count; ++i) {
        if (pos + 46 > central.size()) return false;
        const uint8_t *hdr = central.data() + pos;
        if (rd32(hdr) != 0x02014b50) return false;

        Entry entry;
        entry.method = rd16(hdr + 10);
        entry.compressed_size = rd32(hdr + 20);
        entry.uncompressed_size = rd32(hdr + 24);
        uint16_t name_len = rd16(hdr + 28);
        uint16_t extra_len = rd16(hdr + 30);
        uint16_t comment_len = rd16(hdr + 32);
        entry.local_header_offset = rd32(hdr + 42);

        size_t record_size = 46u + name_len + extra_len + comment_len;
        if (record_size > central.size() - pos) return false;
        std::string name((const char *)hdr + 46, name_len);
        pos += record_size;

        name = normalize_path(name);
        if (!name.empty() && name.back() != '/') {
            _entries[name] = entry;
        }
    }

    return true;
}

std::string EpubZip::index_cache_path(const char *source_path)
{
    char name[48];
    snprintf(name, sizeof(name), "/%016llx.zix", (unsigned long long)path_hash64(source_path));
    return std::string(app_paths::epub_metadata_dir()) + name;
}

bool EpubZip::load_index_cache(const char *source_path)
{
    struct stat source_stat = {};
    if (stat(source_path, &source_stat) != 0) return false;

    std::string cache_path = index_cache_path(source_path);
    struct stat cache_stat = {};
    if (stat(cache_path.c_str(), &cache_stat) != 0 || cache_stat.st_size < 32 ||
        cache_stat.st_size > ZIP_INDEX_MAX_BYTES) return false;

    FILE *fp = std::fopen(cache_path.c_str(), "rb");
    if (!fp) return false;
    std::vector<uint8_t> data((size_t)cache_stat.st_size);
    bool read_ok = std::fread(data.data(), 1, data.size(), fp) == data.size();
    std::fclose(fp);
    if (!read_ok) return false;

    size_t pos = 0;
    auto take32 = [&](uint32_t *value) {
        if (pos + 4 > data.size()) return false;
        *value = rd32(data.data() + pos);
        pos += 4;
        return true;
    };
    auto take64 = [&](uint64_t *value) {
        if (pos + 8 > data.size()) return false;
        *value = rd64(data.data() + pos);
        pos += 8;
        return true;
    };

    uint32_t magic = 0, version = 0, path_len = 0, entry_count = 0;
    uint64_t file_size = 0, mtime = 0;
    if (!take32(&magic) || !take32(&version) || !take64(&file_size) || !take64(&mtime) ||
        !take32(&path_len) || path_len > 4096 || pos + path_len > data.size()) return false;
    std::string saved_path((const char *)data.data() + pos, path_len);
    pos += path_len;
    if (!take32(&entry_count) || entry_count > 100000 || magic != ZIP_INDEX_MAGIC ||
        version != ZIP_INDEX_VERSION || saved_path != source_path ||
        file_size != (uint64_t)source_stat.st_size || mtime != (uint64_t)source_stat.st_mtime) return false;

    std::unordered_map<std::string, Entry> entries;
    entries.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (pos + 2 > data.size()) return false;
        uint16_t name_len = rd16(data.data() + pos);
        pos += 2;
        if (name_len == 0 || pos + name_len + 14 > data.size()) return false;
        std::string name((const char *)data.data() + pos, name_len);
        pos += name_len;
        Entry entry;
        entry.method = rd16(data.data() + pos); pos += 2;
        entry.compressed_size = rd32(data.data() + pos); pos += 4;
        entry.uncompressed_size = rd32(data.data() + pos); pos += 4;
        entry.local_header_offset = rd32(data.data() + pos); pos += 4;
        entries.emplace(std::move(name), entry);
    }
    if (pos != data.size()) return false;
    _entries.swap(entries);
    return !_entries.empty();
}

void EpubZip::save_index_cache(const char *source_path) const
{
    struct stat source_stat = {};
    if (_entries.empty() || stat(source_path, &source_stat) != 0) return;
    app_paths::ensure_dir(app_paths::epub_metadata_dir());

    std::vector<uint8_t> data;
    size_t reserve = 32 + std::strlen(source_path) + _entries.size() * 48;
    data.reserve(reserve);
    append32(&data, ZIP_INDEX_MAGIC);
    append32(&data, ZIP_INDEX_VERSION);
    append64(&data, (uint64_t)source_stat.st_size);
    append64(&data, (uint64_t)source_stat.st_mtime);
    append32(&data, (uint32_t)std::strlen(source_path));
    data.insert(data.end(), source_path, source_path + std::strlen(source_path));
    append32(&data, (uint32_t)_entries.size());
    for (const auto &item : _entries) {
        if (item.first.empty() || item.first.size() > UINT16_MAX) return;
        append16(&data, (uint16_t)item.first.size());
        data.insert(data.end(), item.first.begin(), item.first.end());
        append16(&data, item.second.method);
        append32(&data, item.second.compressed_size);
        append32(&data, item.second.uncompressed_size);
        append32(&data, item.second.local_header_offset);
    }
    if (data.size() > ZIP_INDEX_MAX_BYTES) return;

    std::string cache_path = index_cache_path(source_path);
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
}

bool EpubZip::read_entry_data_offset(FILE *fp, const Entry &entry, uint32_t *offset)
{
    uint8_t hdr[30];
    if (std::fseek(fp, (long)entry.local_header_offset, SEEK_SET) != 0) return false;
    if (std::fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return false;
    if (rd32(hdr) != 0x04034b50) return false;

    uint16_t name_len = rd16(hdr + 26);
    uint16_t extra_len = rd16(hdr + 28);
    *offset = entry.local_header_offset + sizeof(hdr) + name_len + extra_len;
    return true;
}

bool EpubZip::read_entry_to_string(const std::string &name, std::string *out, bool trailing_null)
{
    if (!out) return false;
    out->clear();

    Entry entry;
    if (!entry_info(name, &entry)) {
        ESP_LOGW(TAG, "entry not found: %s", name.c_str());
        return false;
    }
    if (entry.uncompressed_size > 4 * 1024 * 1024) {
        ESP_LOGW(TAG, "entry too large for memory read: %s size=%u", name.c_str(), (unsigned)entry.uncompressed_size);
        return false;
    }

    FILE *fp = std::fopen(_path.c_str(), "rb");
    if (!fp) return false;

    uint32_t data_offset = 0;
    bool ok = read_entry_data_offset(fp, entry, &data_offset);
    if (ok) ok = std::fseek(fp, (long)data_offset, SEEK_SET) == 0;

    std::vector<uint8_t> compressed;
    if (ok) {
        compressed.resize(entry.compressed_size);
        if (entry.compressed_size > 0 &&
            std::fread(compressed.data(), 1, compressed.size(), fp) != compressed.size()) {
            ok = false;
        }
    }
    std::fclose(fp);
    if (!ok) return false;

    std::vector<uint8_t> inflated;
    inflated.resize((size_t)entry.uncompressed_size + (trailing_null ? 1 : 0));

    if (entry.method == 0) {
        if (compressed.size() < entry.uncompressed_size) return false;
        std::memcpy(inflated.data(), compressed.data(), entry.uncompressed_size);
    } else if (entry.method == 8) {
        size_t written = lgfx_tinfl_decompress_mem_to_mem(inflated.data(),
                                                          entry.uncompressed_size,
                                                          compressed.data(),
                                                          compressed.size(),
                                                          0);
        if (written != entry.uncompressed_size) {
            ESP_LOGW(TAG, "inflate failed: %s out=%u/%u",
                     name.c_str(),
                     (unsigned)written,
                     (unsigned)entry.uncompressed_size);
            return false;
        }
    } else {
        ESP_LOGW(TAG, "unsupported method=%u entry=%s", (unsigned)entry.method, name.c_str());
        return false;
    }

    if (trailing_null) inflated[entry.uncompressed_size] = 0;
    out->assign((const char *)inflated.data(), entry.uncompressed_size);
    return true;
}

bool EpubZip::extract_entry_to_file(const std::string &name, const std::string &out_path, uint32_t max_size)
{
    Entry entry;
    if (!entry_info(name, &entry)) {
        ESP_LOGW(TAG, "entry not found: %s", name.c_str());
        return false;
    }
    if (entry.uncompressed_size > max_size) {
        ESP_LOGW(TAG, "entry too large for file extract: %s size=%u", name.c_str(), (unsigned)entry.uncompressed_size);
        return false;
    }

    FILE *fp = std::fopen(_path.c_str(), "rb");
    if (!fp) return false;

    uint32_t data_offset = 0;
    bool ok = read_entry_data_offset(fp, entry, &data_offset);
    if (ok) ok = std::fseek(fp, (long)data_offset, SEEK_SET) == 0;

    if (ok && entry.method == 0) {
        FILE *out = std::fopen(out_path.c_str(), "wb");
        if (!out) {
            std::fclose(fp);
            ESP_LOGW(TAG, "output open failed: %s", out_path.c_str());
            return false;
        }

        uint8_t buf[4096];
        uint32_t remaining = entry.uncompressed_size;
        while (ok && remaining > 0) {
            size_t chunk = std::min<size_t>(sizeof(buf), remaining);
            if (std::fread(buf, 1, chunk, fp) != chunk) {
                ok = false;
                break;
            }
            if (std::fwrite(buf, 1, chunk, out) != chunk) {
                ok = false;
                break;
            }
            remaining -= (uint32_t)chunk;
        }

        std::fclose(out);
        std::fclose(fp);
        return ok;
    }

    std::vector<uint8_t> compressed;
    if (ok) {
        compressed.resize(entry.compressed_size);
        if (entry.compressed_size > 0 &&
            std::fread(compressed.data(), 1, compressed.size(), fp) != compressed.size()) {
            ok = false;
        }
    }
    std::fclose(fp);
    if (!ok) return false;

    std::vector<uint8_t> inflated;
    inflated.resize(entry.uncompressed_size);

    if (entry.method == 0) {
        if (compressed.size() < entry.uncompressed_size) return false;
        std::memcpy(inflated.data(), compressed.data(), entry.uncompressed_size);
    } else if (entry.method == 8) {
        size_t written = lgfx_tinfl_decompress_mem_to_mem(inflated.data(),
                                                          entry.uncompressed_size,
                                                          compressed.data(),
                                                          compressed.size(),
                                                          0);
        if (written != entry.uncompressed_size) {
            ESP_LOGW(TAG, "inflate failed: %s out=%u/%u",
                     name.c_str(),
                     (unsigned)written,
                     (unsigned)entry.uncompressed_size);
            return false;
        }
    } else {
        ESP_LOGW(TAG, "unsupported method=%u entry=%s", (unsigned)entry.method, name.c_str());
        return false;
    }

    FILE *out = std::fopen(out_path.c_str(), "wb");
    if (!out) {
        ESP_LOGW(TAG, "output open failed: %s", out_path.c_str());
        return false;
    }
    bool wrote = inflated.empty() ||
                 std::fwrite(inflated.data(), 1, inflated.size(), out) == inflated.size();
    std::fclose(out);
    return wrote;
}
