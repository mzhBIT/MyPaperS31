#include "ui/NineKeyKeyboard.h"

#include "ui/SystemFont.h"

#include <algorithm>

namespace ui {
namespace {

constexpr int ROWS = 4;
constexpr int COLUMNS = 5;

const char *PINYIN_LABELS[ROWS][COLUMNS] = {
    {"，", "1", "2 ABC", "3 DEF", "删除"},
    {"。", "4 GHI", "5 JKL", "6 MNO", "重输"},
    {"？", "7 PQRS", "8 TUV", "9 WXYZ", "0"},
    {"符号", "123", "空格", "中/EN", "26键"},
};

const char *NUMERIC_LABELS[ROWS][COLUMNS] = {
    {"，", "1", "2", "3", "删除"},
    {"。", "4", "5", "6", "重输"},
    {"？", "7", "8", "9", "0"},
    {"符号", "拼音", "空格", "中/EN", "26键"},
};

const char *SYMBOL_LABELS[ROWS][COLUMNS] = {
    {"！", "@", "#", "%", "删除"},
    {"、", "（", "）", "-", "重输"},
    {"：", "；", "“", "”", "…"},
    {"拼音", "123", "空格", "中/EN", "26键"},
};

const char *const (*labels_for(NineKeyKeyboard::Page page))[COLUMNS]
{
    if (page == NineKeyKeyboard::Page::Numeric) return NUMERIC_LABELS;
    if (page == NineKeyKeyboard::Page::Symbols) return SYMBOL_LABELS;
    return PINYIN_LABELS;
}

NineKeyAction main_action(int row, int column)
{
    if (row == 0 && column == 0) return {NineKeyActionType::Text, 0, "，"};
    if (row == 1 && column == 0) return {NineKeyActionType::Text, 0, "。"};
    if (row == 2 && column == 0) return {NineKeyActionType::Text, 0, "？"};
    if (row == 0 && column >= 1 && column <= 3) {
        return {NineKeyActionType::Digit, static_cast<char>('0' + column), {}};
    }
    if (row == 1 && column >= 1 && column <= 3) {
        return {NineKeyActionType::Digit, static_cast<char>('3' + column), {}};
    }
    if (row == 2 && column >= 1) {
        const char values[] = {'7', '8', '9', '0'};
        return {NineKeyActionType::Digit, values[column - 1], {}};
    }
    if (column == 4 && row == 0) return {NineKeyActionType::Delete, '\0', {}};
    if (column == 4 && row == 1) return {NineKeyActionType::Reset, '\0', {}};
    if (row == 3 && column == 0) return {NineKeyActionType::Symbols, '\0', {}};
    if (row == 3 && column == 1) return {NineKeyActionType::Numeric, '\0', {}};
    if (row == 3 && column == 2) return {NineKeyActionType::Space, '\0', {}};
    if (row == 3 && column == 3) return {NineKeyActionType::Language, '\0', {}};
    if (row == 3 && column == 4) return {NineKeyActionType::Layout, '\0', {}};
    return {};
}

NineKeyAction symbol_action(int row, int column)
{
    if (column == 4 && row == 0) return {NineKeyActionType::Delete, '\0', {}};
    if (column == 4 && row == 1) return {NineKeyActionType::Reset, '\0', {}};
    if (row == 3 && column == 0) return {NineKeyActionType::Symbols, '\0', {}};
    if (row == 3 && column == 1) return {NineKeyActionType::Numeric, '\0', {}};
    if (row == 3 && column == 2) return {NineKeyActionType::Space, '\0', {}};
    if (row == 3 && column == 3) return {NineKeyActionType::Language, '\0', {}};
    if (row == 3 && column == 4) return {NineKeyActionType::Layout, '\0', {}};
    return {NineKeyActionType::Text, 0, SYMBOL_LABELS[row][column]};
}

}  // namespace

NineKeyKeyboard::NineKeyKeyboard(int top, int bottom, int horizontal_padding, int gap)
    : _top(top), _bottom(bottom), _padding(horizontal_padding), _gap(gap)
{
}

void NineKeyKeyboard::reset()
{
    _page = Page::Pinyin;
}

int NineKeyKeyboard::column_width(int display_width, int column) const
{
    // Give the three input columns more room without changing the keyboard's
    // overall bounds. The side action columns use 17% each and the middle
    // columns use 22% each (17 + 22 + 22 + 22 + 17 = 100).
    static constexpr int WEIGHTS[COLUMNS] = {17, 22, 22, 22, 17};
    const int available = std::max(COLUMNS,
        display_width - _padding * 2 - _gap * (COLUMNS - 1));
    int before = 0;
    for (int i = 0; i < column; ++i) before += WEIGHTS[i];
    const int after = before + WEIGHTS[column];
    return std::max(1, available * after / 100 - available * before / 100);
}

int NineKeyKeyboard::column_x(int display_width, int column) const
{
    int x = _padding;
    for (int i = 0; i < column; ++i) {
        x += column_width(display_width, i) + _gap;
    }
    return x;
}

int NineKeyKeyboard::key_height() const
{
    return std::max(1, (_bottom - _top - _gap * (ROWS - 1)) / ROWS);
}

void NineKeyKeyboard::draw_key(M5GFX &display, int row, int column,
                               const std::string &label) const
{
    const int key_w = column_width(display.width(), column);
    const int key_h = key_height();
    const int x = column_x(display.width(), column);
    const int y = _top + row * (key_h + _gap);
    display.fillRoundRect(x, y, key_w, key_h, 7, TFT_WHITE);
    display.drawRoundRect(x, y, key_w, key_h, 7, TFT_BLACK);
    system_font::draw_center(display, label, x + key_w / 2,
                             y + std::max(0, (key_h - system_font::line_height()) / 2),
                             TFT_BLACK);
}

void NineKeyKeyboard::draw(M5GFX &display) const
{
    const auto labels = labels_for(_page);
    for (int row = 0; row < ROWS; ++row) {
        for (int column = 0; column < COLUMNS; ++column) {
            draw_key(display, row, column, labels[row][column]);
        }
    }
}

NineKeyAction NineKeyKeyboard::handle_tap(int x, int y, int display_width) const
{
    if (y < _top || y >= _bottom) return {};
    const int key_h = key_height();
    const int pitch_y = key_h + _gap;
    const int relative_y = y - _top;
    if (x < _padding || relative_y < 0) return {};
    const int row = relative_y / pitch_y;
    if (row < 0 || row >= ROWS || relative_y % pitch_y >= key_h) return {};

    int column = -1;
    for (int candidate = 0; candidate < COLUMNS; ++candidate) {
        const int left = column_x(display_width, candidate);
        const int right = left + column_width(display_width, candidate);
        if (x >= left && x < right) {
            column = candidate;
            break;
        }
    }
    if (column < 0) return {};

    if (_page == Page::Symbols) return symbol_action(row, column);
    NineKeyAction action = main_action(row, column);
    if (_page == Page::Numeric && action.type == NineKeyActionType::Digit) {
        return {NineKeyActionType::Text, 0, std::string(1, action.digit)};
    }
    return action;
}

}  // namespace ui
