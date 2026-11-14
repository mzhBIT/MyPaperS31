#pragma once

#include "ui/ReaderProfile.h"
#include <M5GFX.h>

#include <string>

// Reading standby artwork selection; shared books and RSS retain separate choices.
bool reader_book_cover_enabled(ReaderProfile profile = ReaderProfile::Shared);
const char *reader_book_cover_path(ReaderProfile profile = ReaderProfile::Shared);
bool reader_book_cover_set(const char *path, std::string *message = nullptr,
                                             ReaderProfile profile = ReaderProfile::Shared);
bool reader_book_cover_clear(ReaderProfile profile = ReaderProfile::Shared);
bool reader_book_cover_matches(const char *path);
void reader_book_cover_file_renamed(const char *old_path, const char *new_path);
void reader_book_cover_file_deleted(const char *path);

// Prepare before changing the display. A missing/invalid image returns false
// so standby can fall back to the system wallpaper. No settings are changed.
// PNG alpha is blended over reading_page in a temporary surface, never cached
// to disk with the page underneath. Other formats use the existing image cache.
bool reader_book_cover_prepare(const char *path, int width, int height,
                               lgfx::LGFXBase *reading_page = nullptr);
bool reader_book_cover_draw(lgfx::LGFXBase &gfx, int width, int height);
void reader_book_cover_release();
