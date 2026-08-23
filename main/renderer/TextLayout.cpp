#include "renderer/TextLayout.h"

#include <algorithm>

namespace text_layout {
namespace {

struct Token {
    uint32_t codepoint = 0;
    size_t start = 0;
    size_t end = 0;
    int width = 0;
};

static size_t next_utf8(const std::string &text, size_t pos, uint32_t *codepoint)
{
    if (codepoint) *codepoint = 0;
    if (pos >= text.size()) return pos;

    const uint8_t c0 = (uint8_t)text[pos++];
    if (c0 < 0x80) {
        if (codepoint) *codepoint = c0;
        return pos;
    }

    int continuation = 0;
    uint32_t cp = 0xFFFD;
    if ((c0 & 0xE0) == 0xC0) {
        continuation = 1;
        cp = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        continuation = 2;
        cp = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        continuation = 3;
        cp = c0 & 0x07;
    } else {
        if (codepoint) *codepoint = cp;
        return pos;
    }

    if (pos + (size_t)continuation > text.size()) {
        if (codepoint) *codepoint = 0xFFFD;
        return pos;
    }
    for (int i = 0; i < continuation; ++i) {
        uint8_t cx = (uint8_t)text[pos];
        if ((cx & 0xC0) != 0x80) {
            if (codepoint) *codepoint = 0xFFFD;
            return pos;
        }
        cp = (cp << 6) | (cx & 0x3F);
        ++pos;
    }
    if (codepoint) *codepoint = cp;
    return pos;
}

static bool ascii_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t';
}

static size_t skip_ascii_spaces(const std::string &text, size_t pos)
{
    while (pos < text.size()) {
        uint32_t cp = 0;
        size_t next = next_utf8(text, pos, &cp);
        if (!ascii_space(cp)) break;
        pos = next;
    }
    return pos;
}

static bool line_is_cjk_dominant(const std::vector<Token> &tokens, size_t count)
{
    int glyphs = 0;
    int cjk = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t cp = tokens[i].codepoint;
        if (cp == '\r' || cp == '\n') continue;
        ++glyphs;
        if (is_cjk_or_fullwidth(cp) || is_cjk_punctuation(cp)) ++cjk;
    }
    return glyphs > 1 && cjk * 2 >= glyphs;
}

} // namespace

bool is_cjk_or_fullwidth(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE10 && cp <= 0xFE6F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) ||
           (cp >= 0x20000 && cp <= 0x3FFFF);
}

bool is_cjk_punctuation(uint32_t cp)
{
    return (cp >= 0x3000 && cp <= 0x303F) ||
           cp == 0x2014 || cp == 0x2018 || cp == 0x2019 ||
           cp == 0x201C || cp == 0x201D || cp == 0x2026;
}

bool prohibited_at_line_start(uint32_t cp)
{
    // Closing punctuation, separators, iteration marks and closing quotes.
    switch (cp) {
    case 0x0021: case 0x0025: case 0x0029: case 0x002C:
    case 0x002E: case 0x003A: case 0x003B: case 0x003F:
    case 0x005D: case 0x007D:
    case 0x00B7: case 0x00BB:
    case 0x2014: case 0x2019: case 0x201D: case 0x2025: case 0x2026:
    case 0x2030: case 0x2032: case 0x2033:
    case 0x3001: case 0x3002: case 0x3005:
    case 0x3009: case 0x300B: case 0x300D: case 0x300F:
    case 0x3011: case 0x3015: case 0x3017: case 0x3019:
    case 0x301B: case 0x301E: case 0x301F:
    case 0xFF01: case 0xFF05: case 0xFF09: case 0xFF0C:
    case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF1F:
    case 0xFF3D: case 0xFF5D:
        return true;
    default:
        return false;
    }
}

bool prohibited_at_line_end(uint32_t cp)
{
    // Opening brackets and opening quotation marks.
    switch (cp) {
    case 0x0028: case 0x005B: case 0x007B:
    case 0x00AB:
    case 0x2018: case 0x201C:
    case 0x3008: case 0x300A: case 0x300C: case 0x300E:
    case 0x3010: case 0x3014: case 0x3016: case 0x3018:
    case 0x301A: case 0x301D:
    case 0xFF08: case 0xFF3B: case 0xFF5B:
        return true;
    default:
        return false;
    }
}

std::vector<Line> break_utf8(const std::string &text,
                             const Options &options,
                             MeasureCodepointFn measure,
                             void *measure_context)
{
    std::vector<Line> result;
    if (text.empty()) {
        Line empty;
        empty.paragraph_last = true;
        result.push_back(empty);
        return result;
    }

    const int max_width = std::max(1, options.max_width);
    size_t line_start = 0;
    // Reuse one line-sized scratch buffer for the whole paragraph. Constructing
    // and freeing a vector for every output line fragments the small internal
    // heap on ESP32-P4, even though the data is only temporary.
    std::vector<Token> tokens;
    tokens.reserve(64);
    while (line_start < text.size()) {
        const int line_indent = line_start == 0
                              ? std::max(0, std::min(options.first_line_indent, max_width - 1))
                              : 0;
        const int line_max_width = std::max(1, max_width - line_indent);
        tokens.clear();
        size_t pos = line_start;
        size_t last_space_index = (size_t)-1;
        int width = 0;
        bool hard_break = false;
        size_t hard_break_next = pos;
        uint32_t overflow_cp = 0;
        size_t overflow_start = pos;

        while (pos < text.size()) {
            Token token;
            token.start = pos;
            token.end = next_utf8(text, pos, &token.codepoint);
            if (token.codepoint == '\r' || token.codepoint == '\n') {
                hard_break = true;
                hard_break_next = token.end;
                if (token.codepoint == '\r' && hard_break_next < text.size() &&
                    text[hard_break_next] == '\n') {
                    ++hard_break_next;
                }
                break;
            }
            token.width = measure ? std::max(0, measure(measure_context, token.codepoint)) : 0;
            int spacing = tokens.empty() ? 0 : options.letter_spacing;
            if (!tokens.empty() && width + spacing + token.width > line_max_width) {
                overflow_cp = token.codepoint;
                overflow_start = token.start;
                break;
            }
            width += spacing + token.width;
            tokens.push_back(token);
            if (ascii_space(token.codepoint)) last_space_index = tokens.size() - 1;
            pos = token.end;
        }

        // A single over-wide glyph still has to make progress.
        if (tokens.empty() && !hard_break) {
            Token token;
            token.start = line_start;
            token.end = next_utf8(text, line_start, &token.codepoint);
            token.width = measure ? std::max(0, measure(measure_context, token.codepoint)) : 0;
            tokens.push_back(token);
            pos = token.end;
            overflow_cp = 0;
        }

        size_t take = tokens.size();
        size_t next_start = hard_break ? hard_break_next : pos;
        if (!hard_break && overflow_cp != 0) {
            next_start = overflow_start;
            if (options.prefer_word_break && last_space_index != (size_t)-1 && last_space_index > 0) {
                take = last_space_index;
                next_start = skip_ascii_spaces(text, tokens[last_space_index].start);
            } else if (options.apply_cjk_kinsoku) {
                // Move one or more characters to the next line until neither
                // side of the boundary violates the Chinese line-break rules.
                while (take > 1) {
                    uint32_t next_cp = take < tokens.size() ? tokens[take].codepoint : overflow_cp;
                    uint32_t prev_cp = tokens[take - 1].codepoint;
                    if (!prohibited_at_line_start(next_cp) && !prohibited_at_line_end(prev_cp)) break;
                    --take;
                    next_start = tokens[take].start;
                }
            }
        }

        size_t byte_end = take > 0 ? tokens[take - 1].end : line_start;
        while (take > 0 && ascii_space(tokens[take - 1].codepoint)) {
            --take;
            byte_end = take > 0 ? tokens[take - 1].end : line_start;
        }

        Line line;
        line.byte_start = line_start;
        line.byte_end = byte_end;
        line.text = text.substr(line.byte_start, line.byte_end - line.byte_start);
        line.hard_break = hard_break;
        line.paragraph_last = next_start >= text.size();
        line.justify = !line.paragraph_last && !line.hard_break && line_is_cjk_dominant(tokens, take);
        line.x_offset = line_indent;
        result.push_back(line);

        if (next_start <= line_start) {
            uint32_t ignored = 0;
            next_start = next_utf8(text, line_start, &ignored);
        }
        line_start = next_start;
    }

    return result;
}

} // namespace text_layout
