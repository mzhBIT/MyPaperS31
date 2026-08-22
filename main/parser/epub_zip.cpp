#include "parser/epub_zip.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <vector>

#include <lgfx/utility/lgfx_miniz.h>

static const char *TAG = "EPUB_ZIP";

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

    FILE *fp = std::fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return false;
    }

    _path = path;
    bool ok = load_central_directory(fp);
    std::fclose(fp);
    if (!ok) {
        ESP_LOGE(TAG, "central directory load failed: %s", path);
        close();
        return false;
    }

    ESP_LOGI(TAG, "opened entries=%u path=%s", (unsigned)_entries.size(), path);
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
    _entries.reserve(entry_count);

    for (uint16_t i = 0; i < entry_count; ++i) {
        uint8_t hdr[46];
        if (std::fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return false;
        if (rd32(hdr) != 0x02014b50) return false;

        Entry entry;
        entry.method = rd16(hdr + 10);
        entry.compressed_size = rd32(hdr + 20);
        entry.uncompressed_size = rd32(hdr + 24);
        uint16_t name_len = rd16(hdr + 28);
        uint16_t extra_len = rd16(hdr + 30);
        uint16_t comment_len = rd16(hdr + 32);
        entry.local_header_offset = rd32(hdr + 42);

        std::string name;
        name.resize(name_len);
        if (name_len > 0 && std::fread(&name[0], 1, name_len, fp) != name_len) return false;
        if (std::fseek(fp, (long)extra_len + (long)comment_len, SEEK_CUR) != 0) return false;

        name = normalize_path(name);
        if (!name.empty() && name.back() != '/') {
            _entries[name] = entry;
        }
    }

    return true;
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
