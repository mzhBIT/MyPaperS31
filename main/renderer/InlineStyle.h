#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <vector>

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
    uint16_t font_scale = 0; // 0 inherits the reader's base size; otherwise percent.
};

inline bool has(uint8_t flags, Flag flag)
{
    return (flags & static_cast<uint8_t>(flag)) != 0;
}

inline int scaled(int pixels, int percent)
{
    return (pixels * percent + (pixels < 0 ? -50 : 50)) / 100;
}

inline uint16_t heading_scale(int level)
{
    constexpr uint16_t sizes[] = {100, 200, 150, 117, 100, 83, 67};
    return sizes[std::clamp(level, 0, 6)];
}

inline Span at(const std::vector<Span> &spans, size_t byte, size_t &first)
{
    while (first < spans.size() && spans[first].byte_end <= byte) ++first;
    Span result;
    for (size_t i = first; i < spans.size() && spans[i].byte_start <= byte; ++i) {
        if (byte >= spans[i].byte_end) continue;
        result.flags |= spans[i].flags;
        if (spans[i].font_scale) result.font_scale = spans[i].font_scale;
    }
    return result;
}

inline int scale_of(const Span &style)
{
    return style.font_scale ? style.font_scale : has(style.flags, Small) ? 65 : 100;
}

// Explicit size runs are disjoint. Emphasis and note markers may overlap them.
inline int max_font_scale(const std::vector<Span> &spans, size_t begin, size_t end)
{
    size_t covered = begin;
    int largest = 0;
    for (const auto &span : spans) {
        if (span.byte_start >= end) break;
        if (!span.font_scale || span.byte_end <= begin) continue;
        if (span.byte_start > covered) largest = std::max(largest, 100);
        covered = std::max(covered, span.byte_end);
        largest = std::max(largest, int(span.font_scale));
    }
    if (covered < end) largest = std::max(largest, 100);
    return largest ? largest : 100;
}

} // namespace inline_style
