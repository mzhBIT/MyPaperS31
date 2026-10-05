#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace weread {
// Index raw delimiters once. This preserves the previous tag-boundary rules
// without eight backwards scans of the chapter for each underline.
class AnnotationBoundaryIndex {
public:
    template<class Checkpoint>
    AnnotationBoundaryIndex(const std::string &html, Checkpoint checkpoint) : _html(html)
    {
        for (size_t i = 0; i < html.size(); ++i) {
            if ((i & 4095u) == 0) checkpoint();
            if (html[i] == '<') _opens.push_back(i);
            else if (html[i] == '>') _closes.push_back(i);
        }
    }
    void snap(size_t *start, size_t *end) const
    {
        if (!start || !end || *start >= *end || *end > _html.size()) return;
        if (inside_tag(*start)) {
            const auto close = std::lower_bound(_closes.begin(), _closes.end(), *start);
            if (close == _closes.end() || *close + 1 >= *end) { *start = *end; return; }
            *start = *close + 1;
        }
        if (inside_tag(*end)) *end = previous(_opens, *end);
        const size_t amp = entity_start(*start);
        if (amp != std::string::npos) {
            // HTML entities are short. Never scan to a distant semicolon.
            const size_t limit = std::min(_html.size(), amp + 17);
            for (size_t i = *start; i < limit; ++i) {
                if (_html[i] == ';') {
                    if (i + 1 < *end) *start = i + 1;
                    break;
                }
            }
        }
        const size_t end_amp = entity_start(*end);
        if (end_amp != std::string::npos) *end = end_amp;
    }
private:
    static size_t previous(const std::vector<size_t> &positions, size_t boundary)
    {
        const auto it = std::lower_bound(positions.begin(), positions.end(), boundary);
        return it == positions.begin() ? std::string::npos : *std::prev(it);
    }
    bool inside_tag(size_t boundary) const
    {
        const size_t open = previous(_opens, boundary);
        const size_t close = previous(_closes, boundary);
        return open != std::string::npos && (close == std::string::npos || open > close);
    }
    size_t entity_start(size_t boundary) const
    {
        const size_t begin = boundary > 12 ? boundary - 12 : 0;
        for (size_t i = boundary; i > begin;) {
            const char c = _html[--i];
            if (c == ';') break;
            if (c == '&') return i;
        }
        return std::string::npos;
    }
    const std::string &_html;
    std::vector<size_t> _opens, _closes;
};
} // namespace weread
