#pragma once

#include <string>

namespace cover_thumbnail {

// Generates a fixed-size, baseline YUV420 JPEG suitable for the ESP32-P4
// hardware decoder. Existing valid thumbnails are reused.
bool ensure(const std::string &source_path, const std::string &thumbnail_path);

// Returns the versioned thumbnail path beside an extracted EPUB cover.
std::string cache_path_for(const std::string &source_path);

} // namespace cover_thumbnail
