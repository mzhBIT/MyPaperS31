#pragma once

#include <M5Unified.h>

#include <string>

namespace ui {

enum class NineKeyActionType {
    None,
    Digit,
    Text,
    Delete,
    Reset,
    Symbols,
    Numeric,
    Space,
    Language,
    Layout,
};

struct NineKeyAction {
    NineKeyActionType type = NineKeyActionType::None;
    char digit = '\0';
    std::string text;
};

class NineKeyKeyboard {
public:
    enum class Page { Pinyin, Symbols, Numeric };

    NineKeyKeyboard(int top, int bottom, int horizontal_padding = 16, int gap = 8);

    void reset();
    Page page() const { return _page; }
    void set_page(Page page) { _page = page; }
    void draw(M5GFX &display) const;
    NineKeyAction handle_tap(int x, int y, int display_width) const;

private:
    int _top;
    int _bottom;
    int _padding;
    int _gap;
    Page _page = Page::Pinyin;

    int column_width(int display_width, int column) const;
    int column_x(int display_width, int column) const;
    int key_height() const;
    void draw_key(M5GFX &display, int row, int column,
                  const std::string &label) const;
};

}  // namespace ui
