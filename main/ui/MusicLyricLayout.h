#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace music_lyric_layout {
inline size_t next_char(const std::string &s, size_t p) {
    const unsigned char c = s[p];
    const size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
                     (c & 0xf0) == 0xe0 ? 3 : 4;
    return std::min(s.size(), p + n);
}
inline bool word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '\'';
}
// Prefer whole English words; hard-wrap overlong words at UTF-8 boundaries.
template<class Measure>
std::vector<std::string> wrap(const std::string &text, int width, Measure measure) {
    std::vector<std::string> lines;
    std::string line;
    const auto flush = [&] {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        lines.push_back(line);
        line.clear();
    };
    for (size_t p = 0; p < text.size();) {
        if (text[p] == '\r') { ++p; continue; }
        if (text[p] == '\n') { flush(); ++p; continue; }
        if (text[p] == ' ' || text[p] == '\t') {
            if (!line.empty() && line.back() != ' ') line += ' ';
            ++p; continue;
        }
        size_t end = next_char(text, p);
        if (word_char(static_cast<unsigned char>(text[p]))) {
            while (end < text.size() && word_char(static_cast<unsigned char>(text[end]))) ++end;
        }
        const std::string token = text.substr(p, end - p);
        if (!line.empty() && measure(line + token) > width) flush();
        if (measure(token) <= width) line += token;
        else {
            for (size_t i = 0; i < token.size();) {
                const size_t next = next_char(token, i);
                const std::string glyph = token.substr(i, next - i);
                if (!line.empty() && measure(line + glyph) > width) flush();
                line += glyph;
                i = next;
            }
        }
        p = end;
    }
    if (!line.empty() || lines.empty()) flush();
    return lines;
}

// Center the whole active sentence where possible. Extremely long sentences
// expose successive windows, never ellipsize or mix the next sentence into it.
inline int window_start(int first, int end, int total, int capacity, uint32_t age_ms) {
    capacity = std::max(1, capacity);
    const int count = end - first;
    if (count > capacity) {
        const int pages = (count + capacity - 1) / capacity;
        return first + std::min(int((age_ms / 5000) % pages) * capacity, count - capacity);
    }
    return std::max(0, std::min(first - (capacity - count) / 2, total - capacity));
}
}
