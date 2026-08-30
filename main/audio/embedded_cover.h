#pragma once

#include <string>

namespace embedded_cover {

// Finds an embedded front cover (FLAC PICTURE, ID3 APIC/PIC, or MP4 covr),
// extracts it into the app cache, and returns the cached image path. When no
// embedded art exists, common sidecar names beside the audio file are used.
bool find_or_extract(const char *audio_path, std::string *image_path);

// Returns a per-audio-file cache path for the generated square thumbnail.
std::string square_thumbnail_path(const char *audio_path);

}  // namespace embedded_cover
