#pragma once

#include <string>

struct RecentBook {
    std::string path;
    std::string type;
    std::string title;
    std::string cover_path;
};

bool recent_book_load(RecentBook *book);
bool recent_book_save(const char *path, const char *type, const char *title = nullptr, const char *cover_path = nullptr);
const char *recent_book_base_name(const char *path);
