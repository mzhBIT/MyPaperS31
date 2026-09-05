#include "weread/WereadContentSource.h"

#include "parser/epub_parser.h"
#include "parser/epub_zip.h"
#include "weread/WereadCrypto.h"
#include "weread/WereadStore.h"
#include "wifi/wifi_setting.h"

#include <esp_log.h>
#include <sdkconfig.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace weread {
namespace {

constexpr const char *TAG = "WEREAD_CONTENT";
constexpr uint64_t MAX_TAR_ENTRY_BYTES = 64ULL * 1024 * 1024;

bool file_exists(const std::string &path, bool require_data = true)
{
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           (!require_data || info.st_size > 0);
}

std::string normalized_archive_path(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (path.rfind("../", 0) == 0) path.erase(0, 3);
    return path;
}

bool archive_name_matches(const std::string &entry, const std::string &wanted)
{
    const std::string normalized_entry = normalized_archive_path(entry);
    const std::string normalized_wanted = normalized_archive_path(wanted);
    if (normalized_entry == normalized_wanted) return true;
    return normalized_entry.size() > normalized_wanted.size() &&
           normalized_entry.compare(normalized_entry.size() - normalized_wanted.size(),
                                    normalized_wanted.size(), normalized_wanted) == 0 &&
           normalized_entry[normalized_entry.size() - normalized_wanted.size() - 1] == '/';
}

uint64_t tar_octal(const uint8_t *data, size_t length)
{
    uint64_t value = 0;
    size_t index = 0;
    while (index < length && (data[index] == ' ' || data[index] == '\0')) ++index;
    for (; index < length && data[index] >= '0' && data[index] <= '7'; ++index) {
        if (value > (UINT64_MAX >> 3)) return UINT64_MAX;
        value = (value << 3) | static_cast<uint64_t>(data[index] - '0');
    }
    return value;
}

std::string tar_text(const uint8_t *data, size_t length)
{
    size_t size = 0;
    while (size < length && data[size]) ++size;
    return std::string(reinterpret_cast<const char *>(data), size);
}

bool extract_tar_entry(const std::string &archive_path, const std::string &href,
                       const std::string &out_path)
{
    FILE *archive = std::fopen(archive_path.c_str(), "rb");
    if (!archive) return false;
    uint8_t header[512];
    bool ok = false;
    while (std::fread(header, 1, sizeof(header), archive) == sizeof(header)) {
        bool empty = true;
        for (uint8_t byte : header) if (byte) { empty = false; break; }
        if (empty) break;
        std::string name = tar_text(header, 100);
        const std::string prefix = tar_text(header + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
        const uint64_t size = tar_octal(header + 124, 12);
        if (size == UINT64_MAX || size > MAX_TAR_ENTRY_BYTES) break;
        const bool regular = header[156] == 0 || header[156] == '0';
        if (regular && archive_name_matches(name, href)) {
            FILE *output = std::fopen(out_path.c_str(), "wb");
            if (!output) break;
            uint8_t buffer[4096];
            uint64_t remaining = size;
            ok = true;
            while (remaining) {
                const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(buffer)));
                if (std::fread(buffer, 1, chunk, archive) != chunk ||
                    std::fwrite(buffer, 1, chunk, output) != chunk) { ok = false; break; }
                remaining -= chunk;
            }
            ok = std::fclose(output) == 0 && ok;
            if (!ok) std::remove(out_path.c_str());
            break;
        }
        const uint64_t padded = (size + 511ULL) & ~511ULL;
        if (padded > static_cast<uint64_t>(LONG_MAX) ||
            std::fseek(archive, static_cast<long>(padded), SEEK_CUR) != 0) break;
    }
    std::fclose(archive);
    return ok;
}

bool extract_package_entry(const std::string &package_path, const std::string &href,
                           const std::string &out_path)
{
    FILE *file = std::fopen(package_path.c_str(), "rb");
    uint8_t signature[4] = {};
    const bool read = file && std::fread(signature, 1, sizeof(signature), file) == sizeof(signature);
    if (file) std::fclose(file);
    if (!read) return false;
    if (signature[0] == 'P' && signature[1] == 'K') {
        EpubZip zip;
        if (!zip.open(package_path.c_str())) return false;
        const std::string normalized = normalized_archive_path(href);
        return zip.extract_unique_suffix_to_file(normalized, out_path);
    }
    return extract_tar_entry(package_path, href, out_path);
}

std::string absolute_resource_url(std::string url)
{
    if (url.rfind("//", 0) == 0) return "https:" + url;
    if (!url.empty() && url.front() == '/') return "https://weread.qq.com" + url;
    return url;
}

bool read_file(const std::string &path, std::string *out)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    if (size < 0 || static_cast<unsigned long>(size) > 8UL * 1024 * 1024) {
        std::fclose(file);
        return false;
    }
    out->resize(static_cast<size_t>(size));
    const bool ok = size == 0 || std::fread(out->data(), 1, out->size(), file) == out->size();
    std::fclose(file);
    return ok;
}

void append_escaped(const char *data, size_t size, std::string *out)
{
    for (size_t i = 0; i < size; ++i) {
        switch (data[i]) {
        case '&': *out += "&amp;"; break;
        case '<': *out += "&lt;"; break;
        case '>': *out += "&gt;"; break;
        default: out->push_back(data[i]); break;
        }
    }
}

bool txt_to_xhtml(const std::string &text, std::string *out)
{
    out->clear();
    out->reserve(text.size() + text.size() / 10 + 64);
    *out = "<html><body>";
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        while (end > start && text[end - 1] == '\r') --end;
        if (end > start) {
            *out += "<p>";
            append_escaped(text.data() + start, end - start, out);
            *out += "</p>";
        }
        if (end >= text.size()) break;
        start = end + 1;
    }
    *out += "</body></html>";
    return true;
}

bool chapter_from_epub(const std::string &path, const Chapter &chapter, std::string *out)
{
    if (!file_exists(path)) return false;
    EpubBook book;
    if (!book.open(path.c_str()) || book.spine_count() <= 0) return false;
    const int index = std::clamp(chapter.index - 1, 0, book.spine_count() - 1);
    return book.read_item_to_string(book.spine_item(index).href, out);
}

}  // namespace

bool load_chapter_content(const std::string &book_id, const Chapter &chapter,
                          std::string *xhtml, std::string *error)
{
    if (!xhtml) return false;
    ChapterPayloadKind kind;
    std::string path;
    const std::string epub_path = store::epub_payload_path(book_id);
    if (chapter_from_epub(epub_path, chapter, xhtml)) return true;

    if (!store::chapter_cached(book_id, chapter, &kind, &path)) {
        if (wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS) != ESP_OK) {
            if (error) *error = "chapter is not cached and WiFi connection failed";
            return false;
        }
        HttpClient client;
        Api api(&client);
        ChapterPayload payload;
        std::string fetch_error;
        const bool fetched = api.get_chapter(book_id, chapter, &payload, &fetch_error);
        if (fetched && payload.kind == ChapterPayloadKind::EpubZip) {
            path = epub_path;
            if (!store::save_response_file(path, payload.content.data(), payload.content.size())) {
                fetch_error = "failed to save EPUB payload";
            }
        } else if (fetched && !store::save_chapter(book_id, chapter, payload, &path)) {
            fetch_error = "failed to save chapter cache";
        }
        kind = payload.kind;
        client.close();
        if (!fetched || !fetch_error.empty()) {
            if (error) *error = fetch_error.empty() ? "chapter download failed" : fetch_error;
            return false;
        }
    }

    if (kind == ChapterPayloadKind::EpubZip || path == epub_path) {
        if (chapter_from_epub(path, chapter, xhtml)) return true;
        if (error) *error = "EPUB chapter extraction failed";
        return false;
    }
    std::string content;
    if (!read_file(path, &content)) {
        if (error) *error = "chapter cache read failed";
        return false;
    }
    if (kind == ChapterPayloadKind::Txt) return txt_to_xhtml(content, xhtml);
    *xhtml = std::move(content);
    return true;
}

bool extract_resource(const std::string &book_id, const Chapter *chapter,
                      const std::string &href,
                      const std::string &out_path)
{
    const std::string epub_path = store::epub_payload_path(book_id);
    if (file_exists(epub_path)) {
        EpubBook epub;
        if (epub.open(epub_path.c_str()) && epub.extract_item_to_file(href, out_path)) return true;
    }
    const bool remote = href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0;
    if (!remote) {
        const std::string missing_path = out_path + ".missing";
        if (file_exists(missing_path, false)) return false;
        bool extracted = false;
        bool package_available = false;
        if (chapter && !chapter->resource_url.empty()) {
            const std::string resource_dir = store::book_directory(book_id) + "/resources";
            mkdir(resource_dir.c_str(), 0775);
            const std::string package_key = md5_hex(
                reinterpret_cast<const uint8_t *>(chapter->uid.data()), chapter->uid.size());
            const std::string package_path = resource_dir + "/" + package_key + ".pkg";
            if (!file_exists(package_path) &&
                wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS) == ESP_OK) {
                HttpClient client;
                const std::string url = absolute_resource_url(chapter->resource_url);
                const std::string referer = "https://weread.qq.com/web/reader/" +
                    encode_id(book_id) + "k" + encode_id(chapter->uid);
                HttpResponse response = client.get(url, referer);
                if (response.ok()) {
                    store::save_response_file(package_path, response.data(), response.size());
                    ESP_LOGI(TAG, "chapter assets cached bytes=%u chapter=%s",
                             static_cast<unsigned>(response.size()), chapter->uid.c_str());
                } else {
                    ESP_LOGW(TAG, "chapter assets download failed status=%d chapter=%s",
                             response.status, chapter->uid.c_str());
                }
                client.close();
            }
            if (file_exists(package_path)) {
                package_available = true;
                extracted = extract_package_entry(package_path, href, out_path);
            }
        }
        if (extracted) {
            std::remove(missing_path.c_str());
            return true;
        }
        // Only persist a negative result after a complete local package was
        // searched. Network failures and old catalogs without a tar URL must
        // remain retryable.
        if (package_available) {
            FILE *marker = std::fopen(missing_path.c_str(), "wb");
            if (marker) std::fclose(marker);
        }
        return false;
    }
    if (wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS) != ESP_OK) return false;
    HttpClient client;
    HttpResponse response = client.get(href);
    const bool ok = response.ok() && store::save_response_file(out_path, response.data(), response.size());
    client.close();
    ESP_LOGI(TAG, "image download ok=%d bytes=%u", ok ? 1 : 0, static_cast<unsigned>(response.size()));
    return ok;
}

}  // namespace weread
