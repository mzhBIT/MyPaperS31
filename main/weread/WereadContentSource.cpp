#include "weread/WereadContentSource.h"
#include "weread/WereadOffline.h"
#include "weread/AnnotationBoundaryIndex.h"
#include "utils/CooperativeYield.h"

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
#include <cstdlib>
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

bool parse_annotation_range(const std::string &range, size_t *start, size_t *end)
{
    const size_t dash = range.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= range.size()) return false;
    char *tail = nullptr;
    const unsigned long long first = std::strtoull(range.c_str(), &tail, 10);
    if (!tail || tail != range.c_str() + dash) return false;
    const unsigned long long last = std::strtoull(range.c_str() + dash + 1, &tail, 10);
    if (!tail || *tail || first >= last || last > SIZE_MAX) return false;
    *start = static_cast<size_t>(first);
    *end = static_cast<size_t>(last);
    return true;
}

std::vector<size_t> utf8_offsets(const std::string &text, CooperativeYield &cooperative)
{
    std::vector<size_t> offsets;
    offsets.reserve(text.size() / 2 + 1);
    size_t position = 0;
    while (position < text.size()) {
        if ((offsets.size() & 4095u) == 0) cooperative.ifDue();
        offsets.push_back(position);
        const uint8_t lead = static_cast<uint8_t>(text[position]);
        size_t length = lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4));
        if (position + length > text.size()) length = 1;
        position += length;
    }
    offsets.push_back(text.size());
    return offsets;
}

std::string wrap_annotation_text(const std::string &fragment, const std::string &range,
                                 CooperativeYield &cooperative)
{
    std::string output;
    output.reserve(fragment.size() + 96);
    bool in_tag = false;
    size_t text_start = 0;
    auto append_text = [&](size_t begin, size_t end) {
        if (begin >= end) return;
        bool visible = false;
        for (size_t i = begin; i < end; ++i) {
            if ((i & 4095u) == 0) cooperative.ifDue();
            if (!std::isspace(static_cast<unsigned char>(fragment[i]))) { visible = true; break; }
        }
        if (!visible) {
            output.append(fragment, begin, end - begin);
            return;
        }
        output += "<span class=\"wr-annotation\" data-wr-range=\"";
        output += range;
        output += "\">";
        output.append(fragment, begin, end - begin);
        output += "</span>";
    };
    for (size_t i = 0; i < fragment.size(); ++i) {
        if ((i & 4095u) == 0) cooperative.ifDue();
        if (!in_tag && fragment[i] == '<') {
            append_text(text_start, i);
            in_tag = true;
            text_start = i;
        } else if (in_tag && fragment[i] == '>') {
            output.append(fragment, text_start, i - text_start + 1);
            in_tag = false;
            text_start = i + 1;
        }
    }
    if (in_tag) output.append(fragment, text_start, fragment.size() - text_start);
    else append_text(text_start, fragment.size());
    return output;
}

void inject_annotations(const std::vector<ChapterAnnotation> &annotations, std::string *xhtml)
{
    if (!xhtml || xhtml->empty() || annotations.empty()) return;
    const int64_t begin_us = esp_timer_get_time();
    CooperativeYield cooperative;
    const std::vector<size_t> offsets = utf8_offsets(*xhtml, cooperative);
    const AnnotationBoundaryIndex boundaries(*xhtml, [&] { cooperative.ifDue(); });
    struct Replacement { size_t start; size_t end; std::string range; };
    std::vector<Replacement> replacements;
    replacements.reserve(annotations.size());
    for (const auto &annotation : annotations) {
        cooperative.ifDue();
        size_t start = 0, end = 0;
        if (!parse_annotation_range(annotation.range, &start, &end) || end >= offsets.size()) continue;
        size_t byte_start = offsets[start];
        size_t byte_end = offsets[end];
        boundaries.snap(&byte_start, &byte_end);
        if (byte_start < byte_end) replacements.push_back({byte_start, byte_end, annotation.range});
    }
    std::sort(replacements.begin(), replacements.end(), [](const Replacement &a, const Replacement &b) {
        return a.start < b.start;
    });
    std::string output;
    output.reserve(xhtml->size() + replacements.size() * 96);
    size_t copied = 0;
    for (const auto &replacement : replacements) {
        cooperative.ifDue();
        if (replacement.start < copied || replacement.start >= replacement.end ||
            replacement.end > xhtml->size()) continue;
        output.append(*xhtml, copied, replacement.start - copied);
        output += wrap_annotation_text(
            xhtml->substr(replacement.start, replacement.end - replacement.start), replacement.range,
            cooperative);
        copied = replacement.end;
    }
    output.append(*xhtml, copied, xhtml->size() - copied);
    *xhtml = std::move(output);
    ESP_LOGI(TAG, "underline injection ranges=%u bytes=%u time=%lld ms",
             static_cast<unsigned>(annotations.size()), static_cast<unsigned>(xhtml->size()),
             (esp_timer_get_time() - begin_us) / 1000);
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
    ChapterPayloadKind kind = ChapterPayloadKind::Xhtml;
    std::string path;
    const std::string epub_path = store::epub_payload_path(book_id);
    const bool epub_cached = chapter_from_epub(epub_path, chapter, xhtml);
    const bool chapter_cached = epub_cached || store::chapter_cached(book_id, chapter, &kind, &path);
    if (offline_mode() && !chapter_cached) {
        if (error) *error = "Chapter is not cached; connect online to download";
        return false;
    }
    const bool comments_enabled = store::public_comments_enabled();
    const bool annotations_missing = comments_enabled &&
                                     !store::annotations_cached(book_id, chapter);

    // Chapter payloads and underline ranges have independent cache lifetimes.
    // Older firmware only fetched ranges while downloading a new chapter, so a
    // cached chapter could permanently miss its underline markers.  Backfill a
    // missing range cache without making an already cached chapter unavailable
    // when the device happens to be offline.
    if (!offline_mode() && (!chapter_cached || annotations_missing)) {
        const bool online =
            wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS) == ESP_OK;
        if (!online && !chapter_cached) {
            if (error) *error = "chapter is not cached and WiFi connection failed";
            return false;
        }
        if (!online) {
            ESP_LOGW(TAG, "underline backfill deferred chapter=%s: WiFi unavailable",
                     chapter.uid.c_str());
        } else {
            HttpClient client;
            Api api(&client);
            if (annotations_missing) {
                std::vector<ChapterAnnotation> annotations;
                std::string annotation_error;
                if (api.get_chapter_annotations(book_id, chapter, &annotations, &annotation_error)) {
                    if (!store::save_annotations(book_id, chapter, annotations)) {
                        ESP_LOGW(TAG, "failed to save annotation cache chapter=%s",
                                 chapter.uid.c_str());
                    } else {
                        ESP_LOGI(TAG,
                                 "underline cache backfilled chapter=%s ranges=%u cached_body=%d",
                                 chapter.uid.c_str(), static_cast<unsigned>(annotations.size()),
                                 chapter_cached ? 1 : 0);
                    }
                } else {
                    ESP_LOGW(TAG, "annotation fetch skipped chapter=%s error=%s",
                             chapter.uid.c_str(), annotation_error.c_str());
                }
            }
            if (!chapter_cached) {
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
                if (!fetched || !fetch_error.empty()) {
                    client.close();
                    if (error) {
                        *error = fetch_error.empty() ? "chapter download failed" : fetch_error;
                    }
                    return false;
                }
            }
            client.close();
        }
    }

    if (epub_cached) {
        std::vector<ChapterAnnotation> annotations;
        if (comments_enabled && store::load_annotations(book_id, chapter, &annotations)) {
            inject_annotations(annotations, xhtml);
        }
        return true;
    }
    if (kind == ChapterPayloadKind::EpubZip || path == epub_path) {
        if (chapter_from_epub(path, chapter, xhtml)) {
            std::vector<ChapterAnnotation> annotations;
            if (comments_enabled && store::load_annotations(book_id, chapter, &annotations)) {
                inject_annotations(annotations, xhtml);
            }
            return true;
        }
        if (error) *error = "EPUB chapter extraction failed";
        return false;
    }
    std::string content;
    if (!read_file(path, &content)) {
        if (error) *error = "chapter cache read failed";
        return false;
    }
    if (kind == ChapterPayloadKind::Txt) {
        if (!txt_to_xhtml(content, xhtml)) return false;
    } else {
        *xhtml = std::move(content);
    }
    std::vector<ChapterAnnotation> annotations;
    if (comments_enabled && store::load_annotations(book_id, chapter, &annotations)) {
        inject_annotations(annotations, xhtml);
    }
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
            if (!offline_mode() && !file_exists(package_path) &&
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
    if (offline_mode() || wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS) != ESP_OK) return false;
    HttpClient client;
    HttpResponse response = client.get(href);
    const bool ok = response.ok() && store::save_response_file(out_path, response.data(), response.size());
    client.close();
    ESP_LOGI(TAG, "image download ok=%d bytes=%u", ok ? 1 : 0, static_cast<unsigned>(response.size()));
    return ok;
}

}  // namespace weread
