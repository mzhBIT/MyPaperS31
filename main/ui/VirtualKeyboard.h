#pragma once

#include <M5Unified.h>

#include <cstddef>
#include <string>

namespace ui {

struct VirtualKeyboardLayout {
    int horizontal_padding = 20;
    int input_y = 174;
    int input_height = 72;
    int keys_y = 330;
    int key_height = 78;
    int key_gap = 8;
    int special_keys_y = 706;
    int submit_y = 850;
    int submit_height = 88;
    int submit_horizontal_padding = 80;
};

enum class VirtualKeyboardAction {
    None,
    InputChanged,
    LayoutChanged,
    LanguageChanged,
    Submit,
    LimitReached,
};

class VirtualKeyboard {
public:
    explicit VirtualKeyboard(size_t max_length = 255,
                             const VirtualKeyboardLayout &layout = VirtualKeyboardLayout());

    void reset();
    void set_secret(bool secret);
    void set_placeholder(const std::string &placeholder);
    void set_submit_label(const std::string &label);
    void set_language_toggle(bool enabled, const std::string &label = "CN/EN");
    void set_input_visible(bool visible);
    void set_text(const std::string &text);

    const std::string &text() const;
    bool empty() const;
    bool erase_last();

    void draw(M5GFX &display) const;
    void draw_input(M5GFX &display) const;
    VirtualKeyboardAction handle_tap(int x, int y, int display_width);

private:
    const char *row_text(int row) const;
    bool character_at(int x, int y, int display_width, char *output) const;
    void draw_key(M5GFX &display, int x, int y, int width,
                  const std::string &label) const;

    VirtualKeyboardLayout _layout;
    size_t _max_length;
    std::string _text;
    std::string _placeholder = "Text";
    std::string _submit_label = "Done";
    bool _secret = false;
    bool _show_secret = false;
    bool _upper = false;
    bool _symbols = false;
    bool _language_toggle = false;
    std::string _language_label = "CN/EN";
    bool _input_visible = true;
};

}  // namespace ui
