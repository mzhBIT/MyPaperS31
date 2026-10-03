#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace map_crc {
inline constexpr auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i=0; i<256; ++i) {
        uint32_t c=i;
        for (int j=0; j<8; ++j) c=(c>>1)^((c&1)?0xedb88320u:0);
        t[i]=c;
    }
    return t;
}();
// IEEE CRC32: initialize state with ~0u and complement the final state.
inline uint32_t update(uint32_t state, const void *data, size_t size) {
    const auto *bytes=static_cast<const uint8_t *>(data);
    for(size_t i=0; i<size; ++i) state=table[(state^bytes[i])&255]^(state>>8);
    return state;
}
}
