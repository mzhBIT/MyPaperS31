#pragma once

#define AUDIO_FILE_ACCEPT_EXTENSIONS ".mp3,.flac,.wav,.m4a,.aac,.ogg,.opus"

namespace audio_file_formats {

// Accepts either a bare file name or a complete path.
bool is_supported(const char *path);

}  // namespace audio_file_formats
