#pragma once

#include <M5GFX.h>

#include <string>

enum class ReaderBackgroundResult {
    Ok,
    Unsupported,
    InvalidImage,
    TooSmall,
    CacheFailed,
    SaveFailed,
};

bool reader_background_enabled();
const char *reader_background_path();
ReaderBackgroundResult reader_background_set(const char *path,
                                             int screen_width,
                                             int screen_height,
                                             std::string *message = nullptr);
bool reader_background_clear();
bool reader_background_matches(const char *path);
void reader_background_file_renamed(const char *old_path, const char *new_path);
void reader_background_file_deleted(const char *path);

// Clears the target to white, then draws the configured image with cover
// scaling and center cropping. Returns true when a background was drawn.
bool reader_background_draw(lgfx::LGFXBase &gfx, int width, int height);
