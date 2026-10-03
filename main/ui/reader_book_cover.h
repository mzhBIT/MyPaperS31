#pragma once

#include "ui/ReaderProfile.h"

#include <string>

// Lock-screen book-cover artwork selection. Rendering is intentionally not
// implemented yet; this module only owns the persistent image choice.
bool reader_book_cover_enabled(ReaderProfile profile = ReaderProfile::Shared);
const char *reader_book_cover_path(ReaderProfile profile = ReaderProfile::Shared);
bool reader_book_cover_set(const char *path, std::string *message = nullptr,
                                             ReaderProfile profile = ReaderProfile::Shared);
bool reader_book_cover_clear(ReaderProfile profile = ReaderProfile::Shared);
bool reader_book_cover_matches(const char *path);
void reader_book_cover_file_renamed(const char *old_path, const char *new_path);
void reader_book_cover_file_deleted(const char *path);
