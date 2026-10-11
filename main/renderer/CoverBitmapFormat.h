#pragma once
#include <cstddef>
#include <cstdint>

namespace cover_thumbnail {
// Little-endian, row-major gray4: high nibble is the left pixel, 0=black.
// Header and source identity are versioned separately from the JPEG cache.
struct BitmapHeader {
    uint32_t magic, version, width, height;
    uint32_t source_bytes, source_mtime, payload_bytes, checksum;
};
static_assert(sizeof(BitmapHeader) == 32, "cover bitmap header layout");
inline uint32_t bitmap_checksum(const uint8_t *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i=0;i<size;++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}
inline bool bitmap_header_valid(const BitmapHeader &h, unsigned w, unsigned height,
                                uint32_t source_bytes, uint32_t source_mtime) {
    return w > 0 && w <= 512 && !(w & 1) && height > 0 && height <= 1024
        && h.magic == 0x34435643u && h.version == 1
        && h.width == w && h.height == height
        && h.source_bytes == source_bytes && h.source_mtime == source_mtime
        && h.payload_bytes == size_t(w / 2) * height;
}
} // namespace cover_thumbnail
