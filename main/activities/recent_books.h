#pragma once

#include <string>
#include <vector>

struct RecentBook {
    std::string path;
    std::string type;
    std::string title;
    std::string cover_path;
};

struct RecentBookProgress {
    int percent = 0;
    int chapter = 0;
    int chapter_count = 0;
};

bool recent_book_load(RecentBook *book);
size_t recent_books_load(std::vector<RecentBook> *books);
bool recent_book_save(const char *path, const char *type, const char *title = nullptr, const char *cover_path = nullptr);
bool recent_books_rename_path(const char *old_path, const char *new_path);
RecentBookProgress recent_book_progress(const RecentBook &book);
const char *recent_book_base_name(const char *path);
