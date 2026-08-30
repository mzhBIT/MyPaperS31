#pragma once

#include <string>

namespace cover_thumbnail {

// Generates a fixed-size, baseline YUV420 JPEG suitable for the ESP32-P4
// hardware decoder. Existing valid thumbnails are reused.
bool ensure(const std::string &source_path, const std::string &thumbnail_path);

// Generates a square baseline JPEG for album artwork.
bool ensure_square(const std::string &source_path, const std::string &thumbnail_path);

// Returns the versioned thumbnail path beside an extracted EPUB cover.
std::string cache_path_for(const std::string &source_path);

// Returns the versioned square thumbnail path beside an extracted cover.
std::string square_cache_path_for(const std::string &source_path);

} // namespace cover_thumbnail
