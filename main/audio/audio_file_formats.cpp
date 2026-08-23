#include "audio/audio_file_formats.h"

#include <cstring>
#include <strings.h>

namespace audio_file_formats {

bool is_supported(const char *path)
{
    const char *extension = path ? std::strrchr(path, '.') : nullptr;
    if (extension == nullptr) return false;

    static constexpr const char *EXTENSIONS[] = {
        ".mp3", ".flac", ".wav", ".m4a", ".aac", ".ogg", ".opus",
    };
    for (const char *supported : EXTENSIONS) {
        if (strcasecmp(extension, supported) == 0) return true;
    }
    return false;
}

}  // namespace audio_file_formats
