#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lgfx_epd_page_turn {
constexpr int bands = 4;
constexpr int stagger = 3;
struct Rect { uint16_t x = 0, y = 0, w = 0, h = 0; };
constexpr int phases(int waveform_phases) { return waveform_phases + (bands-1)*stagger; }
constexpr int local_phase(int phase, int logical_band, int direction)
{
    return phase - (direction > 0 ? bands-1-logical_band : logical_band)*stagger;
}

// Copy only whole 16-pixel bus groups: their packed/permuted byte order stays intact.
inline bool valid_rects(const Rect *rects, size_t width, size_t height)
{
    size_t area = 0;
    for (int i=0; i<bands; ++i) {
        const auto &r = rects[i];
        if (!r.w || !r.h || r.x%16 || r.w%16 || r.x+r.w>width || r.y+r.h>height) return false;
        area += size_t(r.w)*r.h;
        for (int j=0; j<i; ++j) {
            const auto &s = rects[j];
            if (r.x<s.x+s.w && s.x<r.x+r.w && r.y<s.y+s.h && s.y<r.y+r.h) return false;
        }
    }
    return area == width*height;
}
inline void compose(uint8_t *out, size_t frame_bytes, size_t width,
                    const uint8_t *base, const uint16_t *map,
                    int waveform_phases, int phase, int direction, const Rect *rects)
{
    std::memset(out, 0, frame_bytes); // Neutral drive, not a white pixel.
    const size_t stride = width/4;
    for (int band=0; band<bands; ++band) {
        const int local = local_phase(phase, band, direction);
        if (local<0 || local>=waveform_phases) continue;
        const auto &r = rects[band];
        const uint8_t *src = base + size_t(map ? map[local] : local)*frame_bytes;
        if (r.x==0 && r.w==width) {
            std::memcpy(out+size_t(r.y)*stride, src+size_t(r.y)*stride, size_t(r.h)*stride);
        } else {
            for (size_t y=r.y; y<size_t(r.y)+r.h; ++y)
                std::memcpy(out+y*stride+r.x/4, src+y*stride+r.x/4, r.w/4);
        }
    }
}
}
