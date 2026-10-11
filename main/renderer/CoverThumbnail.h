#pragma once

#include <string>

namespace lgfx { inline namespace v1 { class LGFXBase; class LGFX_Device; } }

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

// Final-sized shelf pixels, separate files for grid/list; corrupt or stale
// entries return false without changing the destination.
bool draw_bitmap(lgfx::LGFX_Device &gfx, const std::string &source_path,
                 int x, int y, int width, int height);
// Capture the successfully drawn cover before borders/rails are overlaid.
bool save_bitmap(lgfx::LGFXBase &gfx, const std::string &source_path,
                 int x, int y, int width, int height);

} // namespace cover_thumbnail
