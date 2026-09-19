#include "weread/WereadStore.h"

#include "storage/app_paths.h"
#include "weread/WereadCrypto.h"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <nvs.h>
#include <sys/stat.h>

namespace weread::store {
namespace {

constexpr uint32_t SHELF_MAGIC = 0x31535257;   // WRS1
constexpr uint32_t CATALOG_MAGIC = 0x31435257; // WRC1
constexpr uint32_t MANIFEST_MAGIC = 0x31425257; // WRB1
constexpr uint32_t PROGRESS_MAGIC_V1 = 0x31505257; // WRP1
constexpr uint32_t PROGRESS_MAGIC_V2 = 0x32505257; // WRP2: active reading seconds
constexpr uint32_t PROGRESS_MAGIC = 0x33505257; // WRP3: reader offset and summary
constexpr uint32_t ANNOTATION_MAGIC_V1 = 0x31415257; // WRA1
constexpr uint32_t ANNOTATION_MAGIC_V2 = 0x32415257; // WRA2
constexpr uint32_t ANNOTATION_MAGIC_V3 = 0x33415257; // WRA3: comments omit quoted abstract
constexpr uint32_t ANNOTATION_MAGIC = 0x34415257;    // WRA4: explicit comment separators
constexpr uint32_t MAX_ITEMS = 10000;
constexpr uint32_t MAX_STRING = 1024 * 1024;
constexpr const char *PREFERENCES_NAMESPACE = "weread_pref";
constexpr const char *PUBLIC_COMMENTS_KEY = "pub_comments";
int s_public_comments = -1;

bool exists(const std::string &path)
{
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

std::string safe_id(const std::string &id)
{
    bool safe = !id.empty() && id.size() <= 96;
    for (unsigned char c : id) {
        if (!std::isalnum(c) && c != '-' && c != '_') { safe = false; break; }
    }
    if (safe) return id;
    return md5_hex(reinterpret_cast<const uint8_t *>(id.data()), id.size());
}

bool write_u32(FILE *file, uint32_t value) { return std::fwrite(&value, 1, sizeof(value), file) == sizeof(value); }
bool read_u32(FILE *file, uint32_t *value) { return std::fread(value, 1, sizeof(*value), file) == sizeof(*value); }
bool write_u64(FILE *file, uint64_t value) { return std::fwrite(&value, 1, sizeof(value), file) == sizeof(value); }
bool read_u64(FILE *file, uint64_t *value) { return std::fread(value, 1, sizeof(*value), file) == sizeof(*value); }

bool write_string(FILE *file, const std::string &value)
{
    return value.size() <= MAX_STRING && write_u32(file, static_cast<uint32_t>(value.size())) &&
           (value.empty() || std::fwrite(value.data(), 1, value.size(), file) == value.size());
}

bool read_string(FILE *file, std::string *value)
{
    uint32_t size = 0;
    if (!read_u32(file, &size) || size > MAX_STRING) return false;
    value->resize(size);
    return !size || std::fread(value->data(), 1, size, file) == size;
}

bool commit(FILE *file, const std::string &temporary, const std::string &path, bool valid)
{
    if (file) {
        valid = std::fflush(file) == 0 && valid;
        valid = std::fclose(file) == 0 && valid;
    }
    if (!valid) { std::remove(temporary.c_str()); return false; }
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

std::string extension_for_url(const std::string &url)
{
    size_t end = url.find_first_of("?#");
    if (end == std::string::npos) end = url.size();
    size_t dot = url.rfind('.', end);
    if (dot == std::string::npos || end - dot > 6) return ".jpg";
    std::string extension = url.substr(dot, end - dot);
    for (char &c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension == ".jpeg" || extension == ".jpg" || extension == ".png" ||
        extension == ".gif" || extension == ".webp") return extension;
    return ".jpg";
}

}  // namespace

bool ensure_directories()
{
    app_paths::ensure_data_dir();
    mkdir(app_paths::weread_dir(), 0775);
    mkdir(app_paths::weread_books_dir(), 0775);
    mkdir(app_paths::weread_covers_dir(), 0775);
    return true;
}

std::string book_directory(const std::string &book_id)
{
    ensure_directories();
    std::string path = std::string(app_paths::weread_books_dir()) + "/" + safe_id(book_id);
    mkdir(path.c_str(), 0775);
    mkdir((path + "/chapters").c_str(), 0775);
    mkdir((path + "/images").c_str(), 0775);
    mkdir((path + "/annotations").c_str(), 0775);
    return path;
}

bool save_shelf(const std::vector<Book> &books)
{
    ensure_directories();
    const std::string path = std::string(app_paths::weread_dir()) + "/shelf.bin";
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = write_u32(file, SHELF_MAGIC) && write_u32(file, static_cast<uint32_t>(books.size()));
    for (const auto &book : books) {
        ok = ok && write_string(file, book.id) && write_string(file, book.title) &&
             write_string(file, book.author) && write_string(file, book.cover_url) &&
             write_string(file, book.format) && write_string(file, book.progress_chapter_uid) &&
             write_u32(file, static_cast<uint32_t>(book.progress)) && write_u32(file, book.update_time);
        if (!ok) break;
    }
    return commit(file, temporary, path, ok);
}

bool load_shelf(std::vector<Book> *books)
{
    if (!books) return false;
    const std::string path = std::string(app_paths::weread_dir()) + "/shelf.bin";
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t magic = 0, count = 0;
    bool ok = read_u32(file, &magic) && magic == SHELF_MAGIC && read_u32(file, &count) && count <= MAX_ITEMS;
    std::vector<Book> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; ok && i < count; ++i) {
        Book book;
        uint32_t progress = 0;
        ok = read_string(file, &book.id) && read_string(file, &book.title) &&
             read_string(file, &book.author) && read_string(file, &book.cover_url) &&
             read_string(file, &book.format) && read_string(file, &book.progress_chapter_uid) &&
             read_u32(file, &progress) && read_u32(file, &book.update_time);
        book.progress = static_cast<int>(progress);
        if (ok) loaded.push_back(std::move(book));
    }
    std::fclose(file);
    if (!ok) return false;
    *books = std::move(loaded);
    return true;
}

bool save_chapters(const std::string &book_id, const std::vector<Chapter> &chapters,
                   const std::string &format)
{
    const std::string path = book_directory(book_id) + "/toc.bin";
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = write_u32(file, CATALOG_MAGIC) && write_string(file, format) &&
              write_u32(file, static_cast<uint32_t>(chapters.size()));
    for (const auto &chapter : chapters) {
        ok = ok && write_string(file, chapter.uid) && write_string(file, chapter.title) &&
             write_string(file, chapter.resource_url) && write_u32(file, chapter.index) &&
             write_u32(file, chapter.word_count) && write_u32(file, chapter.paid ? 1U : 0U);
        if (!ok) break;
    }
    return commit(file, temporary, path, ok);
}

bool load_chapters(const std::string &book_id, std::vector<Chapter> *chapters,
                   std::string *format)
{
    if (!chapters) return false;
    const std::string path = book_directory(book_id) + "/toc.bin";
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t magic = 0, count = 0;
    std::string stored_format;
    bool ok = read_u32(file, &magic) && magic == CATALOG_MAGIC && read_string(file, &stored_format) &&
              read_u32(file, &count) && count <= MAX_ITEMS;
    std::vector<Chapter> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; ok && i < count; ++i) {
        Chapter chapter;
        uint32_t index = 0, words = 0, paid = 0;
        ok = read_string(file, &chapter.uid) && read_string(file, &chapter.title) &&
             read_string(file, &chapter.resource_url) && read_u32(file, &index) &&
             read_u32(file, &words) && read_u32(file, &paid);
        chapter.index = static_cast<int>(index);
        chapter.word_count = static_cast<int>(words);
        chapter.paid = paid != 0;
        if (ok) loaded.push_back(std::move(chapter));
    }
    std::fclose(file);
    if (!ok) return false;
    *chapters = std::move(loaded);
    if (format) *format = std::move(stored_format);
    return true;
}

std::string manifest_path(const std::string &book_id)
{
    return book_directory(book_id) + "/book.wrbook";
}

bool save_manifest(const Book &book, const std::vector<Chapter> &chapters,
                   const std::string &format)
{
    const std::string path = manifest_path(book.id);
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = write_u32(file, MANIFEST_MAGIC) && write_string(file, book.id) &&
              write_string(file, book.title) && write_string(file, book.author) &&
              write_string(file, book.cover_url) && write_string(file, format) &&
              write_u32(file, static_cast<uint32_t>(chapters.size()));
    for (const auto &chapter : chapters) {
        ok = ok && write_string(file, chapter.uid) && write_string(file, chapter.title) &&
             write_string(file, chapter.resource_url) &&
             write_u32(file, static_cast<uint32_t>(chapter.index)) &&
             write_u32(file, static_cast<uint32_t>(chapter.word_count)) &&
             write_u32(file, chapter.paid ? 1U : 0U);
        if (!ok) break;
    }
    return commit(file, temporary, path, ok);
}

bool load_manifest(const std::string &path, Book *book, std::vector<Chapter> *chapters,
                   std::string *format)
{
    if (!book || !chapters) return false;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t magic = 0, count = 0;
    Book loaded_book;
    std::string loaded_format;
    bool ok = read_u32(file, &magic) && magic == MANIFEST_MAGIC &&
              read_string(file, &loaded_book.id) && read_string(file, &loaded_book.title) &&
              read_string(file, &loaded_book.author) && read_string(file, &loaded_book.cover_url) &&
              read_string(file, &loaded_format) && read_u32(file, &count) && count <= MAX_ITEMS;
    std::vector<Chapter> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; ok && i < count; ++i) {
        Chapter chapter;
        uint32_t index = 0, words = 0, paid = 0;
        ok = read_string(file, &chapter.uid) && read_string(file, &chapter.title) &&
             read_string(file, &chapter.resource_url) && read_u32(file, &index) &&
             read_u32(file, &words) && read_u32(file, &paid);
        chapter.index = static_cast<int>(index);
        chapter.word_count = static_cast<int>(words);
        chapter.paid = paid != 0;
        if (ok) loaded.push_back(std::move(chapter));
    }
    std::fclose(file);
    if (!ok) return false;
    *book = std::move(loaded_book);
    *chapters = std::move(loaded);
    if (format) *format = std::move(loaded_format);
    return true;
}

std::string chapter_path(const std::string &book_id, const Chapter &chapter, ChapterPayloadKind kind)
{
    const char *extension = kind == ChapterPayloadKind::Txt ? ".txt" :
                            kind == ChapterPayloadKind::EpubZip ? ".epub" : ".xhtml";
    return book_directory(book_id) + "/chapters/" + safe_id(chapter.uid) + extension;
}

bool chapter_cached(const std::string &book_id, const Chapter &chapter,
                    ChapterPayloadKind *kind, std::string *path)
{
    for (ChapterPayloadKind candidate : {ChapterPayloadKind::Xhtml, ChapterPayloadKind::Txt,
                                         ChapterPayloadKind::EpubZip}) {
        std::string candidate_path = chapter_path(book_id, chapter, candidate);
        if (!exists(candidate_path)) continue;
        if (kind) *kind = candidate;
        if (path) *path = std::move(candidate_path);
        return true;
    }
    return false;
}

bool save_chapter(const std::string &book_id, const Chapter &chapter,
                  const ChapterPayload &payload, std::string *path)
{
    const std::string destination = chapter_path(book_id, chapter, payload.kind);
    if (!save_response_file(destination, payload.content.data(), payload.content.size())) return false;
    if (path) *path = destination;
    return true;
}

static std::string annotation_path(const std::string &book_id, const Chapter &chapter)
{
    return book_directory(book_id) + "/annotations/" + safe_id(chapter.uid) + ".bin";
}

bool save_annotations(const std::string &book_id, const Chapter &chapter,
                      const std::vector<ChapterAnnotation> &annotations)
{
    const std::string path = annotation_path(book_id, chapter);
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = annotations.size() <= MAX_ITEMS && write_u32(file, ANNOTATION_MAGIC) &&
              write_u32(file, static_cast<uint32_t>(annotations.size()));
    for (const auto &annotation : annotations) {
        ok = ok && write_string(file, annotation.range) &&
             write_string(file, annotation.popup_text) &&
             write_u32(file, static_cast<uint32_t>(std::max(0, annotation.comment_count))) &&
             write_u32(file, static_cast<uint32_t>(annotation.total_count)) &&
             write_u32(file, static_cast<uint32_t>(std::max(0, annotation.max_idx))) &&
             write_u64(file, static_cast<uint64_t>(annotation.synckey)) &&
             write_u32(file, annotation.has_more ? 1u : 0u) &&
             write_u32(file, annotation.comments_loaded ? 1u : 0u);
        if (!ok) break;
    }
    return commit(file, temporary, path, ok);
}

bool load_annotations(const std::string &book_id, const Chapter &chapter,
                      std::vector<ChapterAnnotation> *annotations)
{
    if (!annotations) return false;
    annotations->clear();
    FILE *file = std::fopen(annotation_path(book_id, chapter).c_str(), "rb");
    if (!file) return false;
    uint32_t magic = 0, count = 0;
    bool ok = read_u32(file, &magic) &&
              (magic == ANNOTATION_MAGIC || magic == ANNOTATION_MAGIC_V3 ||
               magic == ANNOTATION_MAGIC_V2 || magic == ANNOTATION_MAGIC_V1) &&
              read_u32(file, &count) && count <= MAX_ITEMS;
    std::vector<ChapterAnnotation> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; ok && i < count; ++i) {
        ChapterAnnotation annotation;
        uint32_t comments = 0;
        ok = read_string(file, &annotation.range) && read_string(file, &annotation.popup_text) &&
             read_u32(file, &comments);
        annotation.comment_count = static_cast<int>(comments);
        if (ok && (magic == ANNOTATION_MAGIC || magic == ANNOTATION_MAGIC_V3 ||
                   magic == ANNOTATION_MAGIC_V2)) {
            uint32_t total = 0, max_idx = 0, has_more = 0, loaded_flag = 0;
            uint64_t synckey = 0;
            ok = read_u32(file, &total) && read_u32(file, &max_idx) &&
                 read_u64(file, &synckey) && read_u32(file, &has_more) &&
                 read_u32(file, &loaded_flag);
            annotation.total_count = static_cast<int32_t>(total);
            annotation.max_idx = static_cast<int>(max_idx);
            annotation.synckey = static_cast<int64_t>(synckey);
            annotation.has_more = has_more != 0;
            annotation.comments_loaded = loaded_flag != 0;
            if (magic != ANNOTATION_MAGIC && annotation.comment_count > 0) {
                // WRA2 starts with a quoted abstract and WRA3 has no reliable
                // boundary between comments. Refetch the small first page once
                // so WRA4 can draw true separators without guessing content.
                annotation.popup_text.clear();
                annotation.comment_count = 0;
                annotation.total_count = -1;
                annotation.max_idx = 0;
                annotation.synckey = 0;
                annotation.has_more = false;
                annotation.comments_loaded = false;
            }
        } else if (ok) {
            // WRA1 could persist a false "No public comments" result when the
            // gateway response was merely unrecognized. Force those entries
            // through the corrected parser once.
            const bool stale_empty = annotation.comment_count == 0 &&
                annotation.popup_text == "No public comments for this underline.";
            if (stale_empty || annotation.comment_count > 0) annotation.popup_text.clear();
            annotation.comments_loaded = !annotation.popup_text.empty();
            annotation.total_count = annotation.comments_loaded ? annotation.comment_count : -1;
            if (!annotation.comments_loaded) annotation.comment_count = 0;
        }
        if (ok) loaded.push_back(std::move(annotation));
    }
    std::fclose(file);
    if (!ok) return false;
    *annotations = std::move(loaded);
    return true;
}

bool annotations_cached(const std::string &book_id, const Chapter &chapter)
{
    return exists(annotation_path(book_id, chapter));
}

bool annotation_text(const std::string &book_id, const Chapter &chapter,
                     const std::string &range, std::string *text)
{
    if (!text) return false;
    std::vector<ChapterAnnotation> annotations;
    if (!load_annotations(book_id, chapter, &annotations)) return false;
    for (const auto &annotation : annotations) {
        if (annotation.range == range) {
            *text = annotation.popup_text;
            return !text->empty();
        }
    }
    return false;
}

bool load_annotation(const std::string &book_id, const Chapter &chapter,
                     const std::string &range, ChapterAnnotation *annotation)
{
    if (!annotation) return false;
    std::vector<ChapterAnnotation> annotations;
    if (!load_annotations(book_id, chapter, &annotations)) return false;
    for (auto &cached : annotations) {
        if (cached.range != range) continue;
        *annotation = std::move(cached);
        return true;
    }
    return false;
}

bool update_annotation(const std::string &book_id, const Chapter &chapter,
                       const ChapterAnnotation &annotation)
{
    std::vector<ChapterAnnotation> annotations;
    if (!load_annotations(book_id, chapter, &annotations)) return false;
    for (auto &cached : annotations) {
        if (cached.range != annotation.range) continue;
        cached = annotation;
        return save_annotations(book_id, chapter, annotations);
    }
    annotations.push_back(annotation);
    return save_annotations(book_id, chapter, annotations);
}

bool public_comments_enabled()
{
    if (s_public_comments >= 0) return s_public_comments != 0;
    s_public_comments = 1;
    nvs_handle_t handle = 0;
    if (nvs_open(PREFERENCES_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t value = 1;
        if (nvs_get_u8(handle, PUBLIC_COMMENTS_KEY, &value) == ESP_OK) {
            s_public_comments = value ? 1 : 0;
        }
        nvs_close(handle);
    }
    return s_public_comments != 0;
}

bool set_public_comments_enabled(bool enabled)
{
    nvs_handle_t handle = 0;
    if (nvs_open(PREFERENCES_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_u8(handle, PUBLIC_COMMENTS_KEY, enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return false;
    s_public_comments = enabled ? 1 : 0;
    return true;
}

std::string cover_path(const std::string &url)
{
    ensure_directories();
    const std::string directory = std::string(app_paths::weread_covers_dir()) + "/" +
        md5_hex(reinterpret_cast<const uint8_t *>(url.data()), url.size());
    mkdir(directory.c_str(), 0775);
    return directory + "/cover" + extension_for_url(url);
}

std::string epub_payload_path(const std::string &book_id)
{
    return book_directory(book_id) + "/content.epub";
}

bool save_response_file(const std::string &path, const uint8_t *data, size_t size)
{
    if ((!data && size) || !size) return false;
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool ok = std::fwrite(data, 1, size, file) == size;
    return commit(file, temporary, path, ok);
}

bool save_pending_progress(const PendingProgress &progress)
{
    ensure_directories();
    const std::string path = std::string(app_paths::weread_dir()) + "/pending_progress.bin";
    PendingProgress merged = progress;
    PendingProgress previous;
    // A progress write is normally delayed, while active time is sampled more
    // often. Preserve unreported time when both records belong to this book.
    if (load_pending_progress(&previous) && previous.book_id == merged.book_id) {
        const uint64_t total = static_cast<uint64_t>(previous.active_seconds) +
                               static_cast<uint64_t>(merged.active_seconds);
        merged.active_seconds = static_cast<uint32_t>(std::min<uint64_t>(total, 24u * 60u * 60u));
    }
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool ok = write_u32(file, PROGRESS_MAGIC) && write_string(file, merged.book_id) &&
                    write_string(file, merged.chapter.uid) && write_string(file, merged.chapter.title) &&
                    write_u32(file, static_cast<uint32_t>(merged.chapter.index)) &&
                    write_u32(file, static_cast<uint32_t>(std::clamp(merged.percent, 0, 100))) &&
                    write_u32(file, merged.active_seconds) &&
                    write_u64(file, merged.chapter_offset) && write_string(file, merged.summary);
    return commit(file, temporary, path, ok);
}

bool load_pending_progress(PendingProgress *progress)
{
    if (!progress) return false;
    const std::string path = std::string(app_paths::weread_dir()) + "/pending_progress.bin";
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t magic = 0, index = 0, percent = 0, active_seconds = 0;
    uint64_t chapter_offset = 0;
    PendingProgress loaded;
    bool ok = read_u32(file, &magic) && (magic == PROGRESS_MAGIC || magic == PROGRESS_MAGIC_V2 ||
                                         magic == PROGRESS_MAGIC_V1) &&
                    read_string(file, &loaded.book_id) && read_string(file, &loaded.chapter.uid) &&
                    read_string(file, &loaded.chapter.title) && read_u32(file, &index) &&
                    read_u32(file, &percent) && percent <= 100;
    if (ok && magic != PROGRESS_MAGIC_V1) ok = read_u32(file, &active_seconds);
    if (ok && magic == PROGRESS_MAGIC) {
        ok = read_u64(file, &chapter_offset) && read_string(file, &loaded.summary);
    }
    std::fclose(file);
    if (!ok) return false;
    loaded.chapter.index = static_cast<int>(index);
    loaded.percent = static_cast<int>(percent);
    loaded.active_seconds = active_seconds;
    loaded.chapter_offset = chapter_offset;
    *progress = std::move(loaded);
    return true;
}

void clear_pending_progress()
{
    const std::string path = std::string(app_paths::weread_dir()) + "/pending_progress.bin";
    std::remove(path.c_str());
}

}  // namespace weread::store
