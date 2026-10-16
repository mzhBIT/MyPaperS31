#pragma once
#include <cstdint>

namespace font_stream_limits {
// Streamed fonts are bounded by addressable file/FreeType offsets, not RAM capacity.
constexpr bool supported(int64_t bytes, uint64_t seek_max, uint64_t ft_max, uint64_t stream_max) {
    return bytes > 0 && uint64_t(bytes) <= seek_max &&
        uint64_t(bytes) <= ft_max && uint64_t(bytes) <= stream_max;
}
}
