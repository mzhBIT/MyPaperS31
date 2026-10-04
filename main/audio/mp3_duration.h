#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t duration_ms;
    uint32_t sample_rate;
    unsigned channels;
    const char *source;
    const char *stage;
    const char *failure;
    uint32_t frames_scanned;
    uint32_t read_count;
    uint64_t read_bytes;
    long first_frame_offset;
} mp3_duration_info_t;
// Optional progress callback can yield and stop an overly expensive scan.
// Does not own/close file; restores its original position on return.
bool mp3_duration_read(FILE *file, mp3_duration_info_t *info,
                       bool (*progress)(void *), void *ctx);
// Quick metadata-only path. Missing Xing/VBRI returns no-duration-tag, not a full scan.
bool mp3_duration_read_tags(FILE *file, mp3_duration_info_t *info,
                            bool (*progress)(void *), void *ctx);
#ifdef __cplusplus
}
#endif
