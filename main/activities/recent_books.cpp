#include "recent_books.h"
#include "storage/app_paths.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

static constexpr size_t RECENT_BOOK_LIMIT = 12;

const char *recent_book_base_name(const char *path)
{
    if (!path || !path[0]) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void trim_line(std::string *s)
{
    if (!s) return;
    while (!s->empty() && (s->back() == '\n' || s->back() == '\r' || s->back() == ' ' || s->back() == '\t')) {
        s->pop_back();
    }
}

static bool file_exists(const std::string &path)
{
    struct stat st = {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

static void normalize_book(RecentBook *book)
{
    if (!book) return;
    trim_line(&book->path);
    trim_line(&book->type);
    trim_line(&book->title);
    trim_line(&book->cover_path);
    if (book->title.empty()) book->title = recent_book_base_name(book->path.c_str());
    if (!book->cover_path.empty() && !file_exists(book->cover_path)) book->cover_path.clear();
    if (book->type.empty()) {
        const char *dot = strrchr(book->path.c_str(), '.');
        if (dot && strcasecmp(dot, ".epub") == 0) book->type = "epub";
        else book->type = "txt";
    }
}

static bool read_line(FILE *fp, std::string *line)
{
    if (!fp || !line) return false;
    line->clear();
    char chunk[256];
    while (std::fgets(chunk, sizeof(chunk), fp)) {
        line->append(chunk);
        if (!line->empty() && line->back() == '\n') break;
    }
    return !line->empty();
}

size_t recent_books_load(std::vector<RecentBook> *books)
{
    if (!books) return 0;
    books->clear();

    FILE *fp = std::fopen(app_paths::recent_books_file(), "rb");
    if (!fp) return 0;

    while (books->size() < RECENT_BOOK_LIMIT) {
        RecentBook book;
        if (!read_line(fp, &book.path)) break;
        if (!read_line(fp, &book.type) || !read_line(fp, &book.title) || !read_line(fp, &book.cover_path)) break;
        normalize_book(&book);
        if (book.path.empty() || !file_exists(book.path)) continue;

        bool duplicate = false;
        for (const auto &saved : *books) {
            if (saved.path == book.path) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) books->push_back(std::move(book));
    }
    std::fclose(fp);
    return books->size();
}

bool recent_book_load(RecentBook *book)
{
    if (!book) return false;
    *book = RecentBook{};
    std::vector<RecentBook> books;
    if (recent_books_load(&books) == 0) return false;
    *book = std::move(books.front());
    return true;
}

bool recent_book_save(const char *path, const char *type, const char *title, const char *cover_path)
{
    if (!path || !path[0] || !type || !type[0]) return false;

    std::vector<RecentBook> books;
    recent_books_load(&books);
    RecentBook current;
    current.path = path;
    current.type = type;
    current.title = (title && title[0]) ? title : recent_book_base_name(path);
    current.cover_path = (cover_path && cover_path[0]) ? cover_path : "";

    for (auto it = books.begin(); it != books.end(); ++it) {
        if (it->path != current.path) continue;
        if ((!title || !title[0]) && !it->title.empty()) current.title = it->title;
        if ((!cover_path || !cover_path[0]) && !it->cover_path.empty()) current.cover_path = it->cover_path;
        books.erase(it);
        break;
    }
    books.insert(books.begin(), std::move(current));
    if (books.size() > RECENT_BOOK_LIMIT) books.resize(RECENT_BOOK_LIMIT);

    app_paths::ensure_data_dir();
    std::string temp_path = std::string(app_paths::recent_books_file()) + ".tmp";
    FILE *fp = std::fopen(temp_path.c_str(), "wb");
    if (!fp) return false;
    bool ok = true;
    for (const auto &book : books) {
        if (std::fprintf(fp, "%s\n%s\n%s\n%s\n",
                         book.path.c_str(), book.type.c_str(), book.title.c_str(), book.cover_path.c_str()) < 0) {
            ok = false;
            break;
        }
    }
    if (std::fclose(fp) != 0) ok = false;
    if (!ok) {
        std::remove(temp_path.c_str());
        return false;
    }
    std::remove(app_paths::recent_books_file());
    if (std::rename(temp_path.c_str(), app_paths::recent_books_file()) != 0) {
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}
