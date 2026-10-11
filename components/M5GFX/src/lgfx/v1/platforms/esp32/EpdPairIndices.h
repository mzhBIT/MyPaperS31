#pragma once
#include <cstddef>
#include <cstdint>

namespace lgfx_epd_pair_indices {
// Two old and two new 4bpp pixels form the existing 64K pair-LUT key.
#if defined(__GNUC__)
__attribute__((optimize("O3")))
#endif
inline void build(uint16_t* indices, const uint8_t* from,
                  const uint8_t* to, size_t count) {
  for (size_t i = 0; i < count; ++i)
    indices[i] = uint16_t((unsigned(from[i]) << 8) | unsigned(to[i]));
}

#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline uint32_t group(const uint16_t* indices, const uint8_t* lut) {
  static constexpr unsigned shifts[8] = {4, 0, 12, 8, 20, 16, 28, 24};
  uint32_t packed = 0;
  for (unsigned pair = 0; pair < 8; ++pair)
    packed |= uint32_t(lut[indices[pair]]) << shifts[pair];
  return packed;
}

// Boundary groups retain neutral drive outside the byte-aligned update area.
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline uint32_t group_range(const uint16_t* indices, const uint8_t* lut,
                            unsigned first, unsigned end) {
  static constexpr unsigned shifts[8] = {4, 0, 12, 8, 20, 16, 28, 24};
  uint32_t packed = 0;
  for (unsigned pair = first; pair < end; ++pair)
    packed |= uint32_t(lut[indices[pair]]) << shifts[pair];
  return packed;
}

inline void build_rows(uint16_t* indices, const uint8_t* from,
                       const uint8_t* to, size_t stride,
                       size_t byte_begin, size_t byte_end,
                       size_t row_begin, size_t row_end) {
  for (size_t y = row_begin; y < row_end; ++y) {
    const size_t offset = y * stride + byte_begin;
    build(indices + offset, from + offset, to + offset, byte_end - byte_begin);
  }
}
}
