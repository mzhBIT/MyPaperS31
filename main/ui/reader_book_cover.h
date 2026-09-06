#pragma once

#include <string>

// Lock-screen book-cover artwork selection. Rendering is intentionally not
// implemented yet; this module only owns the persistent image choice.
bool reader_book_cover_enabled();
const char *reader_book_cover_path();
bool reader_book_cover_set(const char *path, std::string *message = nullptr);
bool reader_book_cover_clear();
bool reader_book_cover_matches(const char *path);
void reader_book_cover_file_renamed(const char *old_path, const char *new_path);
void reader_book_cover_file_deleted(const char *path);
