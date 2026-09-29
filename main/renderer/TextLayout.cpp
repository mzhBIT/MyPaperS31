#include "renderer/TextLayout.h"
#include "renderer/EnglishHyphenationTrie.h"

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

static bool line_is_cjk_dominant(const std::vector<Token> &tokens, size_t start, size_t end)
{
    int glyphs = 0;
    int cjk = 0;
    end = std::min(end, tokens.size());
    for (size_t i = start; i < end; ++i) {
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

namespace {

constexpr uint32_t SOFT_HYPHEN = 0x00AD;
constexpr int64_t INF_COST = INT64_MAX / 8;

struct BreakChoice {
    size_t end = 0;
    size_t next = 0;
    bool append_hyphen = false;
    bool valid = false;
};

static bool ascii_letter(uint32_t cp)
{
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

static char lower_ascii(uint32_t cp)
{
    return cp >= 'A' && cp <= 'Z' ? static_cast<char>(cp + ('a' - 'A'))
                                  : static_cast<char>(cp);
}

struct EnglishTrieState {
    size_t address = 0;
    uint8_t stride = 1;
    size_t child_count = 0;
    const uint8_t *transitions = nullptr;
    const uint8_t *targets = nullptr;
    const uint8_t *levels = nullptr;
    size_t levels_length = 0;
    bool valid = false;
};

static EnglishTrieState decode_english_state(size_t address)
{
    EnglishTrieState state;
    if (address >= en_patterns.size) return state;
    const uint8_t *base = en_patterns.data + address;
    const size_t remaining = en_patterns.size - address;
    size_t pos = 0;
    const uint8_t header = base[pos++];
    const bool has_levels = (header & 0x80u) != 0;
    uint8_t stride = static_cast<uint8_t>((header >> 5) & 0x03u);
    if (stride == 0) stride = 1;
    size_t child_count = header & 0x1Fu;
    if (child_count == 31u) {
        if (pos >= remaining) return EnglishTrieState();
        child_count = base[pos++];
    }

    const uint8_t *levels = nullptr;
    size_t levels_length = 0;
    if (has_levels) {
        if (pos + 1 >= remaining) return EnglishTrieState();
        const uint8_t high = base[pos++];
        const uint8_t low_and_length = base[pos++];
        const size_t offset = (static_cast<size_t>(high) << 4) |
                              (low_and_length >> 4);
        levels_length = low_and_length & 0x0Fu;
        if (offset < 4 || offset + levels_length > en_patterns.size) {
            return EnglishTrieState();
        }
        levels = en_patterns.data + offset - 4;
    }
    if (pos + child_count > remaining) return EnglishTrieState();
    const uint8_t *transitions = base + pos;
    pos += child_count;
    if (pos + child_count * stride > remaining) return EnglishTrieState();

    state.address = address;
    state.stride = stride;
    state.child_count = child_count;
    state.transitions = transitions;
    state.targets = base + pos;
    state.levels = levels;
    state.levels_length = levels_length;
    state.valid = true;
    return state;
}

static int32_t decode_english_delta(const uint8_t *data, uint8_t stride)
{
    if (stride == 1) return static_cast<int8_t>(data[0]);
    if (stride == 2) {
        return static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                    static_cast<uint16_t>(data[1]));
    }
    const int32_t value = (static_cast<int32_t>(data[0]) << 16) |
                          (static_cast<int32_t>(data[1]) << 8) |
                          static_cast<int32_t>(data[2]);
    return value - (1 << 23);
}

static bool english_transition(const EnglishTrieState &state, uint8_t letter,
                               EnglishTrieState *output)
{
    if (!state.valid || !output) return false;
    for (size_t index = 0; index < state.child_count; ++index) {
        if (state.transitions[index] != letter) continue;
        const int32_t delta = decode_english_delta(
            state.targets + index * state.stride, state.stride);
        const int64_t next = static_cast<int64_t>(state.address) + delta;
        if (next < 0 || static_cast<size_t>(next) >= en_patterns.size) return false;
        *output = decode_english_state(static_cast<size_t>(next));
        return output->valid;
    }
    return false;
}

static void mark_english_word_hyphens(const std::vector<Token> &tokens,
                                      size_t start, size_t end,
                                      std::vector<uint8_t> &break_flags)
{
    // English patterns use a 3/3 minimum. Fixed arrays avoid per-word heap
    // traffic while the serialized dictionary remains read-only in flash.
    constexpr size_t MAX_WORD_CHARS = 70;
    const size_t length = end - start;
    if (length < 6 || length > MAX_WORD_CHARS) return;

    uint8_t word[MAX_WORD_CHARS + 2] = {};
    uint8_t scores[MAX_WORD_CHARS + 2] = {};
    word[0] = '.';
    for (size_t i = 0; i < length; ++i) {
        word[i + 1] = static_cast<uint8_t>(lower_ascii(tokens[start + i].codepoint));
    }
    word[length + 1] = '.';
    const size_t augmented_length = length + 2;
    const EnglishTrieState root = decode_english_state(en_patterns.root_offset);
    if (!root.valid) return;

    for (size_t word_start = 0; word_start < augmented_length; ++word_start) {
        EnglishTrieState state = root;
        for (size_t cursor = word_start; cursor < augmented_length; ++cursor) {
            EnglishTrieState next;
            if (!english_transition(state, word[cursor], &next)) break;
            state = next;
            if (!state.levels || state.levels_length == 0) continue;
            size_t offset = 0;
            for (size_t i = 0; i < state.levels_length; ++i) {
                const uint8_t packed = state.levels[i];
                offset += packed / 10;
                const size_t boundary = word_start + offset;
                if (boundary < 2 || boundary + 2 > augmented_length ||
                    boundary >= augmented_length) {
                    continue;
                }
                scores[boundary] = std::max<uint8_t>(scores[boundary], packed % 10);
            }
        }
    }

    for (size_t split = 3; split + 3 <= length; ++split) {
        if ((scores[split + 1] & 1u) != 0) break_flags[start + split] = 1;
    }
}

static void mark_english_hyphens(const std::vector<Token> &tokens,
                                 std::vector<uint8_t> &break_flags)
{
    size_t i = 0;
    while (i < tokens.size()) {
        if (!ascii_letter(tokens[i].codepoint)) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < tokens.size() && ascii_letter(tokens[i].codepoint)) ++i;
        const size_t end = i;
        mark_english_word_hyphens(tokens, start, end, break_flags);
    }
}

static size_t skip_spaces(const std::vector<Token> &tokens, size_t index)
{
    while (index < tokens.size() && ascii_space(tokens[index].codepoint)) ++index;
    return index;
}

static size_t trim_line_end(const std::vector<Token> &tokens, size_t start, size_t end)
{
    while (end > start && (ascii_space(tokens[end - 1].codepoint) ||
                           tokens[end - 1].codepoint == SOFT_HYPHEN)) {
        --end;
    }
    return end;
}

static size_t ascii_space_count(const std::vector<Token> &tokens, size_t start, size_t end)
{
    end = trim_line_end(tokens, start, end);
    size_t count = 0;
    for (size_t i = start; i < end; ++i) {
        if (ascii_space(tokens[i].codepoint)) ++count;
    }
    return count;
}

static int64_t western_line_cost(const std::vector<Token> &tokens,
                                 size_t start, size_t end,
                                 int width, int max_width, bool last_line)
{
    if (last_line) return 0;
    const int64_t remaining = std::max(0, max_width - width);
    const size_t spaces = ascii_space_count(tokens, start, end);
    if (spaces == 0) return remaining * remaining * 4;

    // The renderer permits each word space to grow by approximately one
    // natural space. Penalize candidates exceeding that capacity so the DP
    // will prefer a later Liang breakpoint instead of leaving a ragged edge
    // or creating telephone-pole-sized spaces.
    int64_t stretch_capacity = 0;
    for (size_t i = start; i < end; ++i) {
        if (ascii_space(tokens[i].codepoint)) {
            stretch_capacity += std::max(2, tokens[i].width);
        }
    }
    const int64_t distributed = remaining * remaining /
                                std::max<int64_t>(1, static_cast<int64_t>(spaces));
    if (remaining <= stretch_capacity) return distributed;
    const int64_t excess = remaining - stretch_capacity;
    return distributed + excess * excess * 8;
}

static bool boundary_allowed(const std::vector<Token> &tokens,
                             const std::vector<uint8_t> &english_hyphens,
                             size_t boundary, const Options &options,
                             bool *append_hyphen)
{
    if (append_hyphen) *append_hyphen = false;
    if (boundary == 0 || boundary > tokens.size()) return false;
    if (boundary == tokens.size()) return true;
    const uint32_t left = tokens[boundary - 1].codepoint;
    const uint32_t right = tokens[boundary].codepoint;
    if (ascii_space(left)) return true;
    if (left == SOFT_HYPHEN) {
        if (append_hyphen) *append_hyphen = true;
        return true;
    }
    if (left == '-' || left == 0x2010 || left == 0x2012 || left == 0x2013) return true;
    if (boundary < english_hyphens.size() && english_hyphens[boundary]) {
        if (append_hyphen) *append_hyphen = true;
        return true;
    }
    if ((is_cjk_or_fullwidth(left) || is_cjk_or_fullwidth(right) ||
         is_cjk_punctuation(left) || is_cjk_punctuation(right))) {
        return !options.apply_cjk_kinsoku ||
               (!prohibited_at_line_end(left) && !prohibited_at_line_start(right));
    }
    return false;
}

static int range_width(const std::vector<Token> &tokens, size_t start, size_t end,
                       bool append_hyphen, const Options &options,
                       MeasureCodepointFn measure, MeasureKerningFn measure_kerning,
                       void *context)
{
    end = trim_line_end(tokens, start, end);
    int width = 0;
    uint32_t previous = 0;
    for (size_t i = start; i < end; ++i) {
        const uint32_t cp = tokens[i].codepoint;
        if (cp == SOFT_HYPHEN) continue;
        if (previous != 0) {
            width += options.letter_spacing;
            if (measure_kerning) width += measure_kerning(context, previous, cp);
        }
        width += tokens[i].width;
        previous = cp;
    }
    if (append_hyphen) {
        if (previous != 0) {
            width += options.letter_spacing;
            if (measure_kerning) width += measure_kerning(context, previous, '-');
        }
        width += measure ? std::max(0, measure(context, '-')) : 0;
    }
    return width;
}

static std::string line_text(const std::string &text, const std::vector<Token> &tokens,
                             size_t start, size_t end, bool append_hyphen,
                             size_t *byte_start, size_t *byte_end)
{
    const size_t visible_end = trim_line_end(tokens, start, end);
    *byte_start = start < tokens.size() ? tokens[start].start : text.size();
    *byte_end = visible_end > start ? tokens[visible_end - 1].end : *byte_start;
    std::string out;
    if (*byte_end > *byte_start) out = text.substr(*byte_start, *byte_end - *byte_start);
    for (size_t pos = 0; (pos = out.find("\xC2\xAD", pos)) != std::string::npos;) {
        out.erase(pos, 2);
    }
    if (append_hyphen) out.push_back('-');
    return out;
}

static bool western_dominant(const std::vector<Token> &tokens)
{
    size_t latin = 0;
    size_t cjk = 0;
    for (const auto &token : tokens) {
        if (ascii_letter(token.codepoint)) ++latin;
        else if (is_cjk_or_fullwidth(token.codepoint)) ++cjk;
    }
    return latin >= 4 && latin > cjk;
}

static BreakChoice farthest_fitting(const std::vector<Token> &tokens,
                                    const std::vector<uint8_t> &english_hyphens,
                                    size_t start, int available, const Options &options,
                                    MeasureCodepointFn measure, MeasureKerningFn kerning,
                                    void *context)
{
    BreakChoice best;
    for (size_t end = start + 1; end <= tokens.size(); ++end) {
        bool append_hyphen = false;
        if (!boundary_allowed(tokens, english_hyphens, end, options, &append_hyphen)) continue;
        const int width = range_width(tokens, start, end, append_hyphen, options, measure, kerning, context);
        if (width <= available) {
            best = {end, skip_spaces(tokens, end), append_hyphen, true};
        } else if (best.valid) {
            break;
        }
    }
    if (best.valid) return best;

    // An unbreakable URL/identifier or a glyph wider than the viewport must
    // still make progress.  Split at the largest visible codepoint boundary.
    size_t end = start;
    for (size_t candidate = start + 1; candidate <= tokens.size(); ++candidate) {
        int width = range_width(tokens, start, candidate, false, options, measure, kerning, context);
        if (width > available && end > start) break;
        end = candidate;
        if (width > available) break;
    }
    return {end, end, false, true};
}

static void optimize_western_choices(const std::vector<Token> &tokens,
                                     const std::vector<uint8_t> &english_hyphens,
                                     int max_width, const Options &options,
                                     MeasureCodepointFn measure, MeasureKerningFn kerning,
                                     void *context, std::vector<BreakChoice> &choices,
                                     std::vector<int64_t> &costs)
{
    const size_t count = tokens.size();
    choices.assign(count + 1, BreakChoice());
    costs.assign(count + 1, INF_COST);
    costs[count] = 0;

    // Only positions reached after a legal break can become a line start.
    // Solving the dynamic program for every character in every word makes a
    // long TXT paragraph needlessly expensive on the reader's CPU.
    std::vector<uint8_t> legal_starts(count + 1, 0);
    legal_starts[0] = 1;
    for (size_t boundary = 1; boundary <= count; ++boundary) {
        bool append_hyphen = false;
        if (boundary_allowed(tokens, english_hyphens, boundary, options, &append_hyphen)) {
            legal_starts[skip_spaces(tokens, boundary)] = 1;
        }
    }

    const int64_t hyphen_penalty =
        std::max<int64_t>(1, static_cast<int64_t>(max_width) * max_width / 48);
    for (size_t reverse = count; reverse-- > 0;) {
        const size_t start = reverse;
        if (!legal_starts[start] || ascii_space(tokens[start].codepoint)) continue;
        for (size_t end = start + 1; end <= count; ++end) {
            bool append_hyphen = false;
            if (!boundary_allowed(tokens, english_hyphens, end, options, &append_hyphen)) continue;
            const int width = range_width(tokens, start, end, append_hyphen, options, measure, kerning, context);
            if (width > max_width) {
                if (choices[start].valid) break;
                continue;
            }
            const size_t next = skip_spaces(tokens, end);
            if (costs[next] == INF_COST) continue;
            const int64_t line_cost = western_line_cost(
                tokens, start, end, width, max_width, next == count);
            // Automatic/soft hyphenation is a fallback for an otherwise loose
            // line, not a mechanism for filling every line to its last pixel.
            const int64_t total = line_cost + costs[next] +
                                  (append_hyphen ? hyphen_penalty : 0);
            if (!choices[start].valid || total <= costs[start]) {
                costs[start] = total;
                choices[start] = {end, next, append_hyphen, true};
            }
        }
        if (!choices[start].valid) {
            choices[start] = farthest_fitting(tokens, english_hyphens, start, max_width,
                                              options, measure, kerning, context);
            costs[start] = costs[choices[start].next] == INF_COST ? 0 : costs[choices[start].next];
        }
    }
}

} // namespace

std::vector<Line> break_utf8(const std::string &text,
                             const Options &options,
                             MeasureCodepointFn measure,
                             void *measure_context,
                             MeasureKerningFn measure_kerning,
                             MeasureCodepointAtFn measure_at)
{
    std::vector<Line> result;
    if (text.empty()) {
        Line empty;
        empty.paragraph_last = true;
        result.push_back(empty);
        return result;
    }

    const int max_width = std::max(1, options.max_width);
    size_t segment_start = 0;
    bool first_output_line = true;
    while (segment_start < text.size()) {
        size_t segment_end = segment_start;
        size_t next_segment = text.size();
        bool hard_break = false;
        while (segment_end < text.size()) {
            uint32_t cp = 0;
            const size_t next = next_utf8(text, segment_end, &cp);
            if (cp == '\r' || cp == '\n') {
                hard_break = true;
                next_segment = next;
                if (cp == '\r' && next_segment < text.size() && text[next_segment] == '\n') ++next_segment;
                break;
            }
            segment_end = next;
        }

        std::vector<Token> tokens;
        tokens.reserve((segment_end - segment_start) / 2 + 1);
        for (size_t pos = segment_start; pos < segment_end;) {
            Token token;
            token.start = pos;
            token.end = next_utf8(text, pos, &token.codepoint);
            token.width = token.codepoint == SOFT_HYPHEN
                        ? 0
                        : (measure_at ? std::max(0, measure_at(measure_context, token.codepoint, pos))
                           : (measure ? std::max(0, measure(measure_context, token.codepoint)) : 0));
            tokens.push_back(token);
            pos = token.end;
        }

        if (tokens.empty()) {
            Line empty;
            empty.byte_start = segment_start;
            empty.byte_end = segment_start;
            empty.hard_break = hard_break;
            empty.x_offset = first_output_line ? std::max(0, options.first_line_indent) : 0;
            result.push_back(std::move(empty));
            first_output_line = false;
        } else {
            std::vector<uint8_t> english_hyphens(tokens.size() + 1, 0);
            if (options.hyphenate_english) mark_english_hyphens(tokens, english_hyphens);
            const bool optimize = options.optimize_western_lines && western_dominant(tokens);
            std::vector<BreakChoice> choices;
            std::vector<int64_t> costs;
            if (optimize) {
                optimize_western_choices(tokens, english_hyphens, max_width, options,
                                         measure, measure_kerning, measure_context, choices, costs);
            }

            size_t start = skip_spaces(tokens, 0);
            while (start < tokens.size()) {
                const int indent = first_output_line
                                 ? std::max(0, std::min(options.first_line_indent, max_width - 1)) : 0;
                const int available = std::max(1, max_width - indent);
                BreakChoice choice;
                if (optimize && !first_output_line && start < choices.size()) choice = choices[start];
                if (optimize && first_output_line) {
                    int64_t best_cost = INF_COST;
                    for (size_t end = start + 1; end <= tokens.size(); ++end) {
                        bool append_hyphen = false;
                        if (!boundary_allowed(tokens, english_hyphens, end, options, &append_hyphen)) continue;
                        const int width = range_width(tokens, start, end, append_hyphen, options,
                                                      measure, measure_kerning, measure_context);
                        if (width > available) {
                            if (choice.valid) break;
                            continue;
                        }
                        const size_t next = skip_spaces(tokens, end);
                        if (next >= costs.size() || costs[next] == INF_COST) continue;
                        const int64_t line_cost = western_line_cost(
                            tokens, start, end, width, available, next == tokens.size());
                        const int64_t total = line_cost + costs[next] +
                                              (append_hyphen
                                                   ? std::max<int64_t>(1,
                                                         static_cast<int64_t>(max_width) * max_width / 48)
                                                   : 0);
                        if (!choice.valid || total <= best_cost) {
                            best_cost = total;
                            choice = {end, next, append_hyphen, true};
                        }
                    }
                }
                if (!choice.valid) {
                    choice = farthest_fitting(tokens, english_hyphens, start, available,
                                              options, measure, measure_kerning, measure_context);
                }

                Line line;
                line.text = line_text(text, tokens, start, choice.end, choice.append_hyphen,
                                      &line.byte_start, &line.byte_end);
                line.x_offset = indent;
                line.hard_break = hard_break && choice.next >= tokens.size();
                const size_t visible_end = trim_line_end(tokens, start, choice.end);
                line.justify = choice.next < tokens.size() &&
                               (line_is_cjk_dominant(tokens, start, visible_end) ||
                                ascii_space_count(tokens, start, visible_end) > 0);
                result.push_back(std::move(line));
                first_output_line = false;
                if (choice.next <= start) choice.next = start + 1;
                start = skip_spaces(tokens, choice.next);
            }
        }

        if (!hard_break) break;
        segment_start = next_segment;
        if (segment_start == text.size()) {
            Line empty;
            empty.byte_start = segment_start;
            empty.byte_end = segment_start;
            empty.hard_break = true;
            result.push_back(std::move(empty));
        }
    }

    if (result.empty()) result.push_back(Line());
    for (auto &line : result) line.paragraph_last = false;
    result.back().paragraph_last = true;
    return result;
}

} // namespace text_layout
