#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace text_layout {

static constexpr uint32_t ALGORITHM_VERSION = 6;

// The layout engine deliberately knows nothing about EPUB/TXT/RSS or the
// concrete font backend. Format readers provide normalized UTF-8 paragraphs;
// the renderer provides the codepoint width callback.
using MeasureCodepointFn = int (*)(void *context, uint32_t codepoint);
using MeasureKerningFn = int (*)(void *context, uint32_t left, uint32_t right);

struct Options {
    int max_width = 0;
    int letter_spacing = 0;
    int first_line_indent = 0;
    bool prefer_word_break = true;
    bool apply_cjk_kinsoku = true;
    bool optimize_western_lines = true;
    bool hyphenate_english = true;
};

struct Line {
    std::string text;
    size_t byte_start = 0;
    size_t byte_end = 0;
    bool paragraph_last = false;
    bool hard_break = false;
    bool justify = false;
    int x_offset = 0;
};

bool is_cjk_or_fullwidth(uint32_t codepoint);
bool is_cjk_punctuation(uint32_t codepoint);
bool prohibited_at_line_start(uint32_t codepoint);
bool prohibited_at_line_end(uint32_t codepoint);

std::vector<Line> break_utf8(const std::string &text,
                             const Options &options,
                             MeasureCodepointFn measure,
                             void *measure_context,
                             MeasureKerningFn measure_kerning = nullptr);

} // namespace text_layout
