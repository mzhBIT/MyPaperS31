#include "ui/VirtualKeyboard.h"

#include "ui/SystemFont.h"
#include "ui/Utf8Text.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ui {

VirtualKeyboard::VirtualKeyboard(size_t max_length, const VirtualKeyboardLayout &layout)
    : _layout(layout), _max_length(max_length)
{
}

void VirtualKeyboard::reset()
{
    _text.clear();
    _show_secret = false;
    _upper = false;
    _symbols = false;
}

void VirtualKeyboard::set_secret(bool secret)
{
    _secret = secret;
    if (!secret) _show_secret = false;
}

void VirtualKeyboard::set_placeholder(const std::string &placeholder)
{
    _placeholder = placeholder;
}

void VirtualKeyboard::set_submit_label(const std::string &label)
{
    _submit_label = label;
}

void VirtualKeyboard::set_language_toggle(bool enabled, const std::string &label)
{
    _language_toggle = enabled;
    _language_label = label;
}

void VirtualKeyboard::set_input_visible(bool visible)
{
    _input_visible = visible;
}

void VirtualKeyboard::set_text(const std::string &text)
{
    _text = utf8::truncate_bytes(text, _max_length);
}

const std::string &VirtualKeyboard::text() const
{
    return _text;
}

bool VirtualKeyboard::empty() const
{
    return _text.empty();
}

bool VirtualKeyboard::erase_last()
{
    return utf8::erase_last(&_text);
}

const char *VirtualKeyboard::row_text(int row) const
{
    static const char *LETTER_ROWS[] = {
        "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm",
    };
    static const char *SYMBOL_ROWS[] = {
        "1234567890", "!@#$%^&*()", "-_=+[]{}<>", ".,;:'\"?/\\`~|",
    };
    return _symbols ? SYMBOL_ROWS[row] : LETTER_ROWS[row];
}

void VirtualKeyboard::draw_input(M5GFX &display) const
{
    if (!_input_visible) return;
    int width = display.width();
    int pad = _layout.horizontal_padding;
    display.fillRect(pad - 2, _layout.input_y - 2,
                     width - pad * 2 + 4, _layout.input_height + 4, TFT_WHITE);
    display.drawRoundRect(pad, _layout.input_y, width - pad * 2,
                          _layout.input_height, 7, TFT_BLACK);

    std::string visible;
    if (!_secret || _show_secret) visible = _text;
    else visible.assign(_text.size(), '*');
    if (visible.empty()) visible = _placeholder;
    int action_width = _secret ? 140 : 28;
    visible = system_font::ellipsize(visible, width - pad * 2 - action_width);
    system_font::draw_left(display, visible, pad + 14,
                           _layout.input_y + std::max(
                               0, (_layout.input_height - system_font::line_height()) / 2),
                           TFT_BLACK);
    if (_secret) {
        system_font::draw_right(display, _show_secret ? "Hide" : "Show",
                                width - pad - 14,
                                _layout.input_y + std::max(
                                    0, (_layout.input_height - system_font::line_height()) / 2),
                                TFT_BLACK);
    }
}

void VirtualKeyboard::draw_key(M5GFX &display, int x, int y, int width,
                               const std::string &label) const
{
    display.fillRoundRect(x, y, width, _layout.key_height, 7, TFT_WHITE);
    display.drawRoundRect(x, y, width, _layout.key_height, 7, TFT_BLACK);
    system_font::draw_center(display, label, x + width / 2,
                             y + std::max(
                                 0, (_layout.key_height - system_font::line_height()) / 2),
                             TFT_BLACK);
}

void VirtualKeyboard::draw(M5GFX &display) const
{
    int width = display.width();
    int pad = _layout.horizontal_padding;
    if (_input_visible) draw_input(display);

    for (int row = 0; row < 4; ++row) {
        const char *keys = row_text(row);
        int count = static_cast<int>(strlen(keys));
        int key_width = (width - pad * 2 - (count - 1) * _layout.key_gap) / count;
        int total_width = count * key_width + (count - 1) * _layout.key_gap;
        int start_x = (width - total_width) / 2;
        int y = _layout.keys_y + row * (_layout.key_height + _layout.key_gap);
        for (int column = 0; column < count; ++column) {
            char label[2] = {keys[column], '\0'};
            if (_upper && !_symbols) {
                label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            }
            draw_key(display, start_x + column * (key_width + _layout.key_gap),
                     y, key_width, label);
        }
    }

    const int special_widths[] = {110, 110, 210, 160};
    const char *special_labels[] = {
        _language_toggle ? _language_label.c_str() : (_upper ? "SHIFT" : "Shift"),
        _symbols ? "ABC" : "123", "Space", "Back",
    };
    int total = special_widths[0] + special_widths[1] + special_widths[2] +
                special_widths[3] + _layout.key_gap * 3;
    int x = (width - total) / 2;
    for (int i = 0; i < 4; ++i) {
        draw_key(display, x, _layout.special_keys_y, special_widths[i], special_labels[i]);
        x += special_widths[i] + _layout.key_gap;
    }

    int submit_width = width - _layout.submit_horizontal_padding * 2;
    display.fillRoundRect(_layout.submit_horizontal_padding, _layout.submit_y,
                          submit_width, _layout.submit_height, 9, TFT_BLACK);
    system_font::draw_center(display, _submit_label, width / 2,
                             _layout.submit_y + std::max(
                                 0, (_layout.submit_height - system_font::line_height()) / 2),
                             TFT_WHITE);
}

bool VirtualKeyboard::character_at(int touch_x, int touch_y,
                                   int display_width, char *output) const
{
    if (output == nullptr || touch_y < _layout.keys_y) return false;
    int pitch = _layout.key_height + _layout.key_gap;
    int row = (touch_y - _layout.keys_y) / pitch;
    int within_row = (touch_y - _layout.keys_y) % pitch;
    if (row < 0 || row >= 4 || within_row >= _layout.key_height) return false;

    const char *keys = row_text(row);
    int count = static_cast<int>(strlen(keys));
    int key_width = (display_width - _layout.horizontal_padding * 2 -
                     (count - 1) * _layout.key_gap) /
                    count;
    int total_width = count * key_width + (count - 1) * _layout.key_gap;
    int start_x = (display_width - total_width) / 2;
    for (int column = 0; column < count; ++column) {
        int x = start_x + column * (key_width + _layout.key_gap);
        if (touch_x >= x && touch_x < x + key_width) {
            char value = keys[column];
            if (_upper && !_symbols) {
                value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
            }
            *output = value;
            return true;
        }
    }
    return false;
}

VirtualKeyboardAction VirtualKeyboard::handle_tap(int x, int y, int display_width)
{
    int pad = _layout.horizontal_padding;
    if (_secret && y >= _layout.input_y && y < _layout.input_y + _layout.input_height &&
        x > display_width - pad - 130) {
        _show_secret = !_show_secret;
        return VirtualKeyboardAction::InputChanged;
    }

    char value = '\0';
    if (character_at(x, y, display_width, &value)) {
        if (_text.size() >= _max_length) return VirtualKeyboardAction::LimitReached;
        _text.push_back(value);
        return VirtualKeyboardAction::InputChanged;
    }

    if (y >= _layout.special_keys_y && y < _layout.special_keys_y + _layout.key_height) {
        const int widths[] = {110, 110, 210, 160};
        int total = widths[0] + widths[1] + widths[2] + widths[3] + _layout.key_gap * 3;
        int key_x = (display_width - total) / 2;
        for (int i = 0; i < 4; ++i) {
            if (x >= key_x && x < key_x + widths[i]) {
                if (i == 0) {
                    if (_language_toggle) return VirtualKeyboardAction::LanguageChanged;
                    _upper = !_upper;
                    return VirtualKeyboardAction::LayoutChanged;
                }
                if (i == 1) {
                    _symbols = !_symbols;
                    return VirtualKeyboardAction::LayoutChanged;
                }
                if (i == 2) {
                    if (_text.size() >= _max_length) return VirtualKeyboardAction::LimitReached;
                    _text.push_back(' ');
                    return VirtualKeyboardAction::InputChanged;
                }
                if (erase_last()) return VirtualKeyboardAction::InputChanged;
                return VirtualKeyboardAction::None;
            }
            key_x += widths[i] + _layout.key_gap;
        }
    }

    if (y >= _layout.submit_y && y < _layout.submit_y + _layout.submit_height &&
        x >= _layout.submit_horizontal_padding &&
        x < display_width - _layout.submit_horizontal_padding) {
        return VirtualKeyboardAction::Submit;
    }
    return VirtualKeyboardAction::None;
}

}  // namespace ui
