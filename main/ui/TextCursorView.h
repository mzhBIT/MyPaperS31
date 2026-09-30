#pragma once

#include "ui/SystemFont.h"
#include "ui/Utf8Text.h"

#include <algorithm>
#include <string>

namespace ui {

struct TextCursorView {
    std::string label;
    size_t first = 0;
    size_t last = 0;
    int caret_x = 0;
};

inline std::string cursor_label(const std::string &text, size_t first, size_t last, bool masked)
{
    const std::string part = text.substr(first, last - first);
    return masked ? std::string(utf8::length(part), '*') : part;
}

inline TextCursorView cursor_view(const std::string &text, size_t cursor,
                                  int max_width, bool masked)
{
    TextCursorView view;
    cursor = std::min(cursor, text.size());
    max_width = std::max(0, max_width);
    while (view.first < cursor &&
           system_font::measure(cursor_label(text, view.first, cursor, masked)) > max_width) {
        view.first = utf8::next_boundary(text, view.first);
    }
    view.caret_x = system_font::measure(cursor_label(text, view.first, cursor, masked));
    view.last = cursor;
    while (view.last < text.size()) {
        const size_t next = utf8::next_boundary(text, view.last);
        if (system_font::measure(cursor_label(text, view.first, next, masked)) > max_width) break;
        view.last = next;
    }
    view.label = cursor_label(text, view.first, view.last, masked);
    return view;
}

inline size_t cursor_at_x(const std::string &text, const TextCursorView &view,
                          int relative_x, bool masked)
{
    if (relative_x <= 0) return view.first;
    for (size_t pos = view.first; pos < view.last;) {
        const size_t next = utf8::next_boundary(text, pos);
        const int left = system_font::measure(cursor_label(text, view.first, pos, masked));
        const int right = system_font::measure(cursor_label(text, view.first, next, masked));
        if (relative_x < (left + right) / 2) return pos;
        pos = next;
    }
    return view.last;
}

}  // namespace ui
