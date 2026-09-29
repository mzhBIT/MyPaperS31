#pragma once

#include <cstddef>
#include <cstdint>

namespace inline_style {

enum Flag : uint8_t {
    None = 0,
    Bold = 1u << 0,
    Italic = 1u << 1,
    Code = 1u << 2,
    Small = 1u << 3,
};

struct Span {
    size_t byte_start = 0;
    size_t byte_end = 0;
    uint8_t flags = None;
};

inline bool has(uint8_t flags, Flag flag)
{
    return (flags & static_cast<uint8_t>(flag)) != 0;
}

} // namespace inline_style
