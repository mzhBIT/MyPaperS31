#include "file_duration.h"
#include "mp3_duration.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <climits>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace {
uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t le32(const uint8_t *p) {
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}
uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint64_t le64(const uint8_t *p) { return uint64_t(le32(p + 4)) << 32 | le32(p); }
uint32_t milliseconds(uint64_t units, uint32_t rate) {
    if (!rate) return 0;
    // Split the quotient to avoid overflow on a corrupt 64-bit duration.
    if (units / rate > UINT32_MAX / 1000) return UINT32_MAX;
    const uint64_t ms = units / rate * 1000 + ((units % rate) * 1000 + rate / 2) / rate;
    return ms > UINT32_MAX ? UINT32_MAX : uint32_t(ms);
}

struct Reader {
    FILE *file;
    bool (*progress)(void *);
    void *context;
    uint64_t size = 0, base = 0;
    size_t cached = 0;
    uint8_t *buffer = nullptr;
    static constexpr size_t capacity = 32768;
    Reader(FILE *f, bool (*p)(void *), void *c) : file(f), progress(p), context(c) {
        if (fseek(f, 0, SEEK_END) == 0) {
            const long end = ftell(f);
            if (end > 0) size = uint64_t(end);
        }
#ifdef ESP_PLATFORM
        buffer = static_cast<uint8_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
        buffer = static_cast<uint8_t *>(malloc(capacity));
#endif
    }
    ~Reader() { free(buffer); }
    bool tick() { return !progress || progress(context); }
    bool read(uint64_t offset, void *out, size_t length) {
        if (!buffer || offset > size || length > size - offset || !tick()) return false;
        auto *dest = static_cast<uint8_t *>(out);
        while (length) {
            if (!cached || offset < base || offset >= base + cached) {
                base = offset;
                if (offset > LONG_MAX || fseek(file, long(offset), SEEK_SET) != 0) return false;
                cached = fread(buffer, 1, size_t(std::min<uint64_t>(capacity, size - offset)), file);
                if (!cached) return false;
            }
            const size_t n = std::min(length, cached - size_t(offset - base));
            memcpy(dest, buffer + size_t(offset - base), n);
            dest += n; offset += n; length -= n;
        }
        return true;
    }
    bool after_id3(uint64_t *offset) {
        uint8_t p[10];
        if (!read(0, p, sizeof(p))) return false;
        *offset = 0;
        if (memcmp(p, "ID3", 3) != 0) return true;
        if ((p[6] | p[7] | p[8] | p[9]) & 0x80) return false;
        *offset = 10 + (uint32_t(p[6]) << 21 | uint32_t(p[7]) << 14 |
                         uint32_t(p[8]) << 7 | p[9]) + ((p[5] & 0x10) ? 10 : 0);
        return *offset <= size;
    }
};

uint32_t flac(Reader &r) {
    uint64_t p; uint8_t h[8], info[34];
    if (!r.after_id3(&p) || !r.read(p, h, sizeof(h)) || memcmp(h, "fLaC", 4) ||
        (h[4] & 0x7f) != 0 || ((uint32_t(h[5]) << 16) | (uint32_t(h[6]) << 8) | h[7]) != 34 ||
        !r.read(p + 8, info, sizeof(info))) return 0;
    const uint32_t rate = uint32_t(info[10]) << 12 | uint32_t(info[11]) << 4 | info[12] >> 4;
    const uint64_t samples = uint64_t(info[13] & 15) << 32 | be32(info + 14);
    return milliseconds(samples, rate);
}

uint32_t wav(Reader &r) {
    uint8_t h[16];
    if (!r.read(0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
    uint32_t rate = 0, byte_rate = 0, align = 0, format = 0;
    uint64_t data = 0, samples = 0;
    for (uint64_t p = 12; p + 8 <= r.size;) {
        if (!r.read(p, h, 8)) return 0;
        const uint32_t n = le32(h + 4);
        if (n > r.size - p - 8) return 0;
        if (!memcmp(h, "fmt ", 4)) {
            if (n < 16 || !r.read(p + 8, h, 16)) return 0;
            format = h[0] | uint32_t(h[1]) << 8;
            rate = le32(h + 4); byte_rate = le32(h + 8);
            align = h[12] | uint32_t(h[13]) << 8;
        } else if (!memcmp(h, "fact", 4) && n >= 4) {
            if (!r.read(p + 8, h, 4)) return 0;
            samples = le32(h);
        } else if (!memcmp(h, "data", 4)) data += n;
        p += 8 + uint64_t(n) + (n & 1);
    }
    if (samples && rate) return milliseconds(samples, rate);
    if ((format == 1 || format == 3 || format == 0xfffe) && align && rate)
        return milliseconds(data / align, rate);
    return milliseconds(data, byte_rate);
}

struct Atom { uint64_t payload, end; uint32_t type; };
bool atom(Reader &r, uint64_t start, uint64_t limit, Atom *a) {
    uint8_t h[16];
    if (limit > r.size || start > limit || limit - start < 8 || !r.read(start, h, 8)) return false;
    uint64_t n = be32(h); unsigned header = 8;
    a->type = be32(h + 4);
    if (n == 1) {
        if (limit - start < 16 || !r.read(start + 8, h + 8, 8)) return false;
        n = be64(h + 8); header = 16;
    } else if (!n) n = limit - start;
    if (n < header || n > limit - start) return false;
    a->payload = start + header; a->end = start + n;
    return true;
}
constexpr uint32_t tag(const char *s) {
    return uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 | uint32_t(s[3]);
}
uint32_t media_duration(Reader &r, const Atom &mdia) {
    bool audio = false; uint32_t duration = 0;
    for (uint64_t p = mdia.payload; p < mdia.end;) {
        Atom a; uint8_t h[32];
        if (!atom(r, p, mdia.end, &a)) return 0;
        if (a.type == tag("hdlr")) {
            if (a.end - a.payload < 12 || !r.read(a.payload, h, 12)) return 0;
            audio = be32(h + 8) == tag("soun");
        } else if (a.type == tag("mdhd")) {
            if (a.end - a.payload < 4 || !r.read(a.payload, h, 4)) return 0;
            const unsigned n = h[0] == 1 ? 32 : 20;
            if (h[0] > 1 || a.end - a.payload < n || !r.read(a.payload, h, n)) return 0;
            const uint32_t rate = be32(h + (h[0] == 1 ? 20 : 12));
            const uint64_t units = h[0] == 1 ? be64(h + 24) : be32(h + 16);
            if (units != (h[0] == 1 ? UINT64_MAX : UINT32_MAX)) duration = milliseconds(units, rate);
        }
        p = a.end;
    }
    return audio ? duration : 0;
}
uint32_t mp4(Reader &r) {
    for (uint64_t p = 0; p < r.size;) {
        Atom moov;
        if (!atom(r, p, r.size, &moov)) return 0;
        if (moov.type == tag("moov")) {
            for (uint64_t t = moov.payload; t < moov.end;) {
                Atom track;
                if (!atom(r, t, moov.end, &track)) return 0;
                if (track.type == tag("trak")) {
                    for (uint64_t m = track.payload; m < track.end;) {
                        Atom media;
                        if (!atom(r, m, track.end, &media)) return 0;
                        if (media.type == tag("mdia")) {
                            const uint32_t duration = media_duration(r, media);
                            if (duration) return duration;
                        }
                        m = media.end;
                    }
                }
                t = track.end;
            }
        }
        p = moov.end;
    }
    return 0;
}

uint32_t ogg(Reader &r) {
    // Follow one audio logical stream at a time; add sequential chained streams.
    uint32_t serial = 0, rate = 0, skip = 0;
    uint64_t last = 0, total_ms = 0;
    bool active = false, completed = false;
    for (uint64_t p = 0; p < r.size;) {
        uint8_t h[27], lacing[255], id[19];
        if (!r.read(p, h, 27) || memcmp(h, "OggS", 4) || h[4] != 0 ||
            !r.read(p + 27, lacing, h[26])) return 0;
        size_t payload = 0;
        for (unsigned i = 0; i < h[26]; ++i) payload += lacing[i];
        const uint64_t body = p + 27 + h[26];
        if (payload > r.size - body) return 0;
        const uint32_t current = le32(h + 14);
        if (!active && (h[5] & 2) && payload >= sizeof(id)) {
            if (!r.read(body, id, sizeof(id))) return 0;
            rate = 0; skip = 0;
            if (!memcmp(id, "OpusHead", 8)) {
                rate = 48000; skip = id[10] | uint32_t(id[11]) << 8;
            } else if (id[0] == 1 && !memcmp(id + 1, "vorbis", 6)) rate = le32(id + 12);
            if (rate) { active = true; serial = current; last = 0; }
        }
        if (active && current == serial) {
            const uint64_t granule = le64(h + 6);
            if (granule != UINT64_MAX) last = granule;
            if (h[5] & 4) {
                total_ms += milliseconds(last > skip ? last - skip : 0, rate);
                active = false; completed = true;
            }
        }
        p = body + payload;
    }
    // A truncated stream has no trustworthy end position.
    return !active && completed ? uint32_t(std::min<uint64_t>(UINT32_MAX, total_ms)) : 0;
}

uint32_t aac(Reader &r) {
    static constexpr uint32_t rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    uint64_t p, samples = 0; uint32_t rate = 0;
    if (!r.after_id3(&p)) return 0;
    while (p < r.size) {
        uint8_t h[7];
        // Optional ID3v1 trailer.
        if (r.size - p == 128 && r.read(p, h, 3) && !memcmp(h, "TAG", 3)) break;
        if (!r.read(p, h, 7) || h[0] != 0xff || (h[1] & 0xf6) != 0xf0) return 0;
        const unsigned index = (h[2] >> 2) & 15;
        const unsigned length = unsigned(h[3] & 3) << 11 | unsigned(h[4]) << 3 | h[5] >> 5;
        if (index >= sizeof(rates) / sizeof(rates[0]) || length < ((h[1] & 1) ? 7u : 9u) ||
            length > r.size - p || (rate && rate != rates[index])) return 0;
        rate = rates[index];
        samples += 1024 * (1 + (h[6] & 3));
        p += length;
    }
    return milliseconds(samples, rate);
}
} // namespace

bool audio_file_duration_read(FILE *file, const char *path, uint32_t *duration_ms,
                              bool (*progress)(void *), void *context,
                              AudioFileDurationDetails *details) {
    if (details) *details = {};
    if (!file || !path || !duration_ms) return false;
    *duration_ms = 0;
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    const long saved = ftell(file);
    if (saved < 0) return false;
    if (!strcasecmp(ext, ".mp3")) {
        mp3_duration_info_t info = {};
        if (mp3_duration_read(file, &info, progress, context)) *duration_ms = info.duration_ms;
        if (details) {
            details->source = info.source ? info.source : "unavailable";
            details->stage = info.stage ? info.stage : "none";
            details->failure = info.failure ? info.failure : "none";
            details->frames = info.frames_scanned;
            details->reads = info.read_count;
            details->first_frame = info.first_frame_offset;
        }
    } else {
        Reader r(file, progress, context);
        if (!strcasecmp(ext, ".flac")) *duration_ms = flac(r);
        else if (!strcasecmp(ext, ".wav")) *duration_ms = wav(r);
        else if (!strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".mp4")) *duration_ms = mp4(r);
        else if (!strcasecmp(ext, ".ogg") || !strcasecmp(ext, ".opus")) *duration_ms = ogg(r);
        else if (!strcasecmp(ext, ".aac")) *duration_ms = aac(r);
    }
    clearerr(file);
    if (fseek(file, saved, SEEK_SET) != 0 || (progress && !progress(context))) *duration_ms = 0;
    return *duration_ms != 0;
}
