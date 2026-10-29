#include "mp3_duration.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define READ_BUFFER_SIZE 32768
// SD seeks/small reads are expensive. Scan many frames in each sequential window.
#define FRAME_SCAN_WINDOW READ_BUFFER_SIZE
typedef struct {
    FILE *file;
    unsigned char *buffer;
    long base, end;
    size_t size;
    bool (*progress)(void *);
    void *ctx;
    mp3_duration_info_t *info;
    bool frame_scan;
    bool tags_only;
} reader_t;
typedef struct { unsigned rate, samples, bytes, channels, version, crc; } frame_t;

static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static bool read_at(reader_t *r, long pos, unsigned char *out, size_t n) {
    if (pos < 0 || pos > r->end || n > (size_t)(r->end - pos)) return false;
    while (n) {
        if (pos < r->base || pos >= r->base + (long)r->size) {
            if (r->progress && !r->progress(r->ctx)) {
                r->info->failure = "budget/cancel"; return false;
            }
            // Sector-align windows; adjacent windows normally continue without a seek.
            const long base = pos - pos % 512;
            if (ftell(r->file) != base && fseek(r->file, base, SEEK_SET) != 0) return false;
            r->base = base;
            r->size = fread(r->buffer, 1, r->frame_scan ? FRAME_SCAN_WINDOW : READ_BUFFER_SIZE, r->file);
            ++r->info->read_count;
            r->info->read_bytes += r->size;
            if (!r->size) return false;
        }
        const size_t offset = (size_t)(pos - r->base);
        size_t take = r->size - offset;
        if (take > n) take = n;
        memcpy(out, r->buffer + offset, take);
        out += take; pos += (long)take; n -= take;
    }
    return true;
}
static bool parse_header(const unsigned char *p, frame_t *f) {
    static const unsigned rates[] = {44100, 48000, 32000};
    static const unsigned br1[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const unsigned br2[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    const unsigned version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;
    const unsigned bitrate = p[2] >> 4, rate = (p[2] >> 2) & 3;
    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0 || version == 1 || layer != 1 ||
        !bitrate || bitrate == 15 || rate == 3 || (p[3] & 3) == 2) return false;
    f->version = version;
    f->rate = rates[rate] / (version == 3 ? 1 : version == 2 ? 2 : 4);
    f->samples = version == 3 ? 1152 : 576;
    f->bytes = (version == 3 ? 144000 : 72000) *
               (version == 3 ? br1[bitrate] : br2[bitrate]) / f->rate + ((p[2] >> 1) & 1);
    f->channels = (p[3] >> 6) == 3 ? 1 : 2;
    f->crc = !(p[1] & 1);
    return f->bytes >= 24;
}
static bool compatible(const frame_t *a, const frame_t *b) {
    return a->version == b->version && a->rate == b->rate && a->channels == b->channels;
}
static bool duration_from_frames(uint32_t frames, const frame_t *f, long audio_bytes,
                                 mp3_duration_info_t *info, const char *source) {
    if (!frames || frames > (uint64_t)audio_bytes / 24 + 1) return false;
    const uint64_t ms = (uint64_t)frames * f->samples * 1000 / f->rate;
    if (!ms) return false;
    info->duration_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    info->sample_rate = f->rate; info->channels = f->channels; info->source = source;
    return true;
}
static bool tagged_duration(reader_t *r, long pos, const frame_t *f,
                            mp3_duration_info_t *info) {
    unsigned char tag[18];
    const unsigned side = f->version == 3 ? (f->channels == 1 ? 17 : 32)
                                         : (f->channels == 1 ? 9 : 17);
    for (unsigned crc = 0; crc <= f->crc; ++crc) {
        const unsigned offset = 4 + side + crc * 2;
        if (offset + 12 > f->bytes || !read_at(r, pos + offset, tag, 12)) continue;
        if ((!memcmp(tag, "Xing", 4) || !memcmp(tag, "Info", 4)) && (be32(tag + 4) & 1) &&
            duration_from_frames(be32(tag + 8), f, r->end - pos, info, "mp3-xing")) return true;
    }
    if (36 + sizeof(tag) <= f->bytes && read_at(r, pos + 36, tag, sizeof(tag)) &&
        !memcmp(tag, "VBRI", 4) && tag[4] == 0 && tag[5] == 1 &&
        duration_from_frames(be32(tag + 14), f, r->end - pos, info, "mp3-vbri")) return true;
    return false;
}
static bool parse(reader_t *r, mp3_duration_info_t *info) {
    info->stage = "metadata";
    unsigned char h[32];
    long pos = 0;
    // Skip whole ID3v2 tags, including large attached album covers and v2.4 footer.
    while (read_at(r, pos, h, 10) && !memcmp(h, "ID3", 3)) {
        if (h[3] < 2 || h[3] > 4 || (h[6] | h[7] | h[8] | h[9]) & 0x80) return false;
        const uint32_t n = (uint32_t)h[6] << 21 | (uint32_t)h[7] << 14 |
                           (uint32_t)h[8] << 7 | h[9];
        const uint32_t skip = 10 + n + (h[3] == 4 && (h[5] & 0x10) ? 10 : 0);
        if (skip > (uint64_t)(r->end - pos)) return false;
        pos += skip;
    }
    if (r->end >= 128 && read_at(r, r->end - 128, h, 3) && !memcmp(h, "TAG", 3)) r->end -= 128;
    if (r->end >= 32 && read_at(r, r->end - 32, h, 32) && !memcmp(h, "APETAGEX", 8)) {
        const uint32_t n = le32(h + 12);
        if (n >= 32 && n <= (uint64_t)(r->end - pos)) {
            // APE tag size includes items + footer, but EXCLUDES the optional
            // 32-byte header. Leaving it behind makes a complete MP3 fail its
            // final frame validation (common with foobar2000-tagged files).
            long start = r->end - (long)n;
            if (le32(h + 20) & 0x80000000u) {
                unsigned char header[32];
                if (start - pos < 32 || !read_at(r, start - 32, header, sizeof(header)) ||
                    memcmp(header, "APETAGEX", 8) || le32(header + 8) != le32(h + 8) ||
                    le32(header + 12) != n || !(le32(header + 20) & 0x20000000u)) {
                    info->failure = "invalid-ape-header";
                    return false;
                }
                start -= 32;
            }
            r->end = start;
        }
    }
    const long search_end = r->end - pos > 1048576 ? pos + 1048576 : r->end;
    frame_t first = {0}, next;
    bool found = false;
    info->stage = "first-frame";
    for (; pos + 4 <= search_end; ++pos) {
        if (!read_at(r, pos, h, 4)) return false;
        if (!parse_header(h, &first) || first.bytes > (uint64_t)(r->end - pos)) continue;
        const long after = pos + first.bytes;
        if (after == r->end || (read_at(r, after, h, 4) && parse_header(h, &next) && compatible(&first, &next))) {
            found = true; break;
        }
    }
    if (!found) return false;
    info->first_frame_offset = pos;
    info->stage = "duration-tag";
    if (tagged_duration(r, pos, &first, info)) return true;
    if (r->tags_only) { info->failure = "no-duration-tag"; return false; }
    // Count validated frames rather than estimating VBR duration from one bitrate.
    const long audio_start = pos;
    uint32_t count = 0;
    info->stage = "frame-scan";
    r->frame_scan = true;
    while (pos + 4 <= r->end) {
        if (!read_at(r, pos, h, 4)) return false;
        if (!parse_header(h, &next) || !compatible(&first, &next)) return false;
        if (next.bytes > (uint64_t)(r->end - pos)) break; // Ignore a truncated last frame.
        ++count; info->frames_scanned = count; pos += next.bytes;
    }
    // Arbitrary garbage in the middle must not yield a misleading partial duration.
    if (r->end - pos > 4096) return false;
    return duration_from_frames(count, &first, r->end - audio_start, info, "mp3-frame-scan");
}
static bool read_duration(FILE *file, mp3_duration_info_t *info,
                          bool (*progress)(void *), void *ctx, bool tags_only) {
    if (!file || !info) return false;
    memset(info, 0, sizeof(*info));
    info->first_frame_offset = -1;
    const long saved = ftell(file);
    if (saved < 0 || fseek(file, 0, SEEK_END)) return false;
    reader_t r = {.file = file, .base = -1, .end = ftell(file), .progress = progress, .ctx = ctx,
                  .info = info, .tags_only = tags_only};
#ifdef ESP_PLATFORM
    r.buffer = heap_caps_malloc(FRAME_SCAN_WINDOW, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    r.buffer = malloc(FRAME_SCAN_WINDOW);
#endif
    const bool ok = r.buffer && r.end > 0 && parse(&r, info);
    free(r.buffer);
    const bool restored = fseek(file, saved, SEEK_SET) == 0;
    if (!ok || !restored) {
        info->duration_ms = 0; info->sample_rate = 0; info->channels = 0; info->source = NULL;
        if (!info->failure) info->failure = !restored ? "restore" : !r.buffer ? "memory" : "invalid-stream";
    }
    return ok && restored;
}
bool mp3_duration_read(FILE *file, mp3_duration_info_t *info,
                       bool (*progress)(void *), void *ctx) {
    return read_duration(file, info, progress, ctx, false);
}
bool mp3_duration_read_tags(FILE *file, mp3_duration_info_t *info,
                            bool (*progress)(void *), void *ctx) {
    return read_duration(file, info, progress, ctx, true);
}
