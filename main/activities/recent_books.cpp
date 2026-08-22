#include "recent_books.h"
#include "storage/app_paths.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

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

bool recent_book_load(RecentBook *book)
{
    if (!book) return false;
    book->path.clear();
    book->type.clear();
    book->title.clear();
    book->cover_path.clear();

    FILE *fp = std::fopen(app_paths::recent_books_file(), "rb");
    if (!fp) return false;

    char line[384] = {};
    if (std::fgets(line, sizeof(line), fp)) book->path = line;
    if (std::fgets(line, sizeof(line), fp)) book->type = line;
    if (std::fgets(line, sizeof(line), fp)) book->title = line;
    if (std::fgets(line, sizeof(line), fp)) book->cover_path = line;
    std::fclose(fp);

    trim_line(&book->path);
    trim_line(&book->type);
    trim_line(&book->title);
    trim_line(&book->cover_path);
    if (book->path.empty() || !file_exists(book->path)) return false;
    if (book->title.empty()) book->title = recent_book_base_name(book->path.c_str());
    if (!book->cover_path.empty() && !file_exists(book->cover_path)) book->cover_path.clear();
    if (book->type.empty()) {
        const char *dot = strrchr(book->path.c_str(), '.');
        if (dot && strcasecmp(dot, ".epub") == 0) book->type = "epub";
        else book->type = "txt";
    }
    return true;
}

bool recent_book_save(const char *path, const char *type, const char *title, const char *cover_path)
{
    if (!path || !path[0] || !type || !type[0]) return false;
    app_paths::ensure_data_dir();
    FILE *fp = std::fopen(app_paths::recent_books_file(), "wb");
    if (!fp) return false;
    std::fprintf(fp,
                 "%s\n%s\n%s\n%s\n",
                 path,
                 type,
                 (title && title[0]) ? title : recent_book_base_name(path),
                 (cover_path && cover_path[0]) ? cover_path : "");
    std::fclose(fp);
    return true;
}
