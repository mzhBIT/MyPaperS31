#include "weread/WereadStore.h"

#include "storage/app_paths.h"
#include "weread/WereadCrypto.h"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>

namespace weread::store {
namespace {

constexpr uint32_t SHELF_MAGIC = 0x31535257;   // WRS1
constexpr uint32_t CATALOG_MAGIC = 0x31435257; // WRC1
constexpr uint32_t MANIFEST_MAGIC = 0x31425257; // WRB1
constexpr uint32_t PROGRESS_MAGIC = 0x31505257; // WRP1
constexpr uint32_t MAX_ITEMS = 10000;
constexpr uint32_t MAX_STRING = 1024 * 1024;

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
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool ok = write_u32(file, PROGRESS_MAGIC) && write_string(file, progress.book_id) &&
                    write_string(file, progress.chapter.uid) && write_string(file, progress.chapter.title) &&
                    write_u32(file, static_cast<uint32_t>(progress.chapter.index)) &&
                    write_u32(file, static_cast<uint32_t>(std::clamp(progress.percent, 0, 100)));
    return commit(file, temporary, path, ok);
}

bool load_pending_progress(PendingProgress *progress)
{
    if (!progress) return false;
    const std::string path = std::string(app_paths::weread_dir()) + "/pending_progress.bin";
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t magic = 0, index = 0, percent = 0;
    PendingProgress loaded;
    const bool ok = read_u32(file, &magic) && magic == PROGRESS_MAGIC &&
                    read_string(file, &loaded.book_id) && read_string(file, &loaded.chapter.uid) &&
                    read_string(file, &loaded.chapter.title) && read_u32(file, &index) &&
                    read_u32(file, &percent) && percent <= 100;
    std::fclose(file);
    if (!ok) return false;
    loaded.chapter.index = static_cast<int>(index);
    loaded.percent = static_cast<int>(percent);
    *progress = std::move(loaded);
    return true;
}

void clear_pending_progress()
{
    const std::string path = std::string(app_paths::weread_dir()) + "/pending_progress.bin";
    std::remove(path.c_str());
}

}  // namespace weread::store
