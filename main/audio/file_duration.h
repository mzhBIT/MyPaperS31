#pragma once
#include <cstdint>
#include <cstdio>

struct AudioFileDurationDetails {
    const char *source = "unavailable";
    const char *stage = "metadata";
    const char *failure = "unknown";
    uint32_t frames = 0;
    uint32_t reads = 0;
    long first_frame = -1;
};

// Reads metadata or scans encoded frame headers; never decodes PCM.
// Restores the caller's FILE position. progress may yield/cancel a long scan.
bool audio_file_duration_read(FILE *file, const char *path, uint32_t *duration_ms,
                              bool (*progress)(void *), void *context,
                              AudioFileDurationDetails *details = nullptr);
