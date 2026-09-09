#include "ui/TextInputOverlay.h"

#include "activities/Activity.h"
#include "ime/PinyinEngine.h"
#include "ui/NineKeyKeyboard.h"
#include "ui/SystemFont.h"
#include "ui/Utf8Text.h"
#include "ui/VirtualKeyboard.h"

#include <M5Unified.h>
#include <nvs.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace ui {
namespace {

constexpr int CANDIDATE_SLOTS = 3;
constexpr int PINYIN_SLOTS = 3;
constexpr int MODE_KEY_X = 20;
constexpr int MODE_KEY_W = 116;
constexpr int MODE_KEY_GAP = 8;
constexpr int LAYOUT_KEY_W = 80;
constexpr int PAGE_KEY_W = 36;
constexpr int PINYIN_DIGITS_W = 92;
constexpr int PINYIN_PAGE_KEY_W = 40;
constexpr const char *INPUT_NVS_NAMESPACE = "text_input";
constexpr const char *INPUT_NVS_LAYOUT = "cn_layout";

bool load_nine_key_preference()
{
    nvs_handle_t handle = 0;
    if (nvs_open(INPUT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t value = 0;
    const bool nine_key = nvs_get_u8(handle, INPUT_NVS_LAYOUT, &value) == ESP_OK && value == 1;
    nvs_close(handle);
    return nine_key;
}

void save_nine_key_preference(bool nine_key)
{
    nvs_handle_t handle = 0;
    if (nvs_open(INPUT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, INPUT_NVS_LAYOUT, nine_key ? 1 : 0) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

VirtualKeyboardLayout overlay_keyboard_layout()
{
    const int height = M5.Display.height();
    VirtualKeyboardLayout layout;
    layout.horizontal_padding = 16;
    layout.key_height = 70;
    layout.key_gap = 8;
    layout.special_keys_y = std::max(820, height - layout.key_height - 20);
    layout.keys_y = layout.special_keys_y -
                    4 * (layout.key_height + layout.key_gap);
    layout.input_y = 0;
    layout.input_height = 0;
    layout.submit_y = height;
    layout.submit_height = 0;
    return layout;
}

}  // namespace

struct TextInputOverlay::Impl {
    TextInputOptions options;
    TextInputCallback submit_callback;
    std::function<void()> cancel_callback;
    VirtualKeyboardLayout layout;
    std::unique_ptr<VirtualKeyboard> keyboard;
    std::unique_ptr<NineKeyKeyboard> nine_keyboard;
    ime::PinyinEngine engine{CANDIDATE_SLOTS};
    ime::Pinyin9Engine nine_engine{CANDIDATE_SLOTS};
    std::string committed;
    std::string message;
    bool chinese = true;
    bool nine_key = false;
    bool shown = false;

    int candidate_y() const { return layout.keys_y - 144; }
    int candidate_h() const { return 136; }
    int candidate_controls_y() const { return candidate_y() + 72; }
    int candidate_controls_h() const { return candidate_h() - 76; }
    int dialog_h() const { return 220; }
    int dialog_y() const { return std::max(84, candidate_y() - dialog_h() - 14); }
    int editor_y() const { return dialog_y() + 56; }
    int editor_h() const { return 64; }
    int buttons_y() const { return dialog_y() + 140; }
    int buttons_h() const { return 60; }
    int overlay_top() const { return std::max(0, dialog_y() - 14); }

    size_t committed_limit() const
    {
        return options.max_bytes > options.fixed_suffix.size()
                   ? options.max_bytes - options.fixed_suffix.size() : 0;
    }

    const std::string &active_composition() const
    {
        return nine_key ? nine_engine.composition() : engine.composition();
    }

    bool active_previous_page()
    {
        return nine_key ? nine_engine.previous_page() : engine.previous_page();
    }

    bool active_next_page()
    {
        return nine_key ? nine_engine.next_page() : engine.next_page();
    }

    std::string active_candidate(size_t slot) const
    {
        return nine_key ? nine_engine.candidate_on_page(slot)
                        : engine.candidate_on_page(slot);
    }

    void clear_composition()
    {
        if (nine_key) nine_engine.clear();
        else {
            keyboard->set_text("");
            engine.set_composition("");
        }
    }

    void update_composition()
    {
        engine.set_composition(keyboard->text());
        keyboard->set_text(engine.composition());
    }

    void append_committed(const std::string &text)
    {
        if (!utf8::append(&committed, text, committed_limit())) {
            message = "Text is too long";
        }
    }

    void commit_candidate(size_t slot)
    {
        std::string candidate = active_candidate(slot);
        if (candidate.empty()) candidate = active_composition();
        if (!candidate.empty()) append_committed(candidate);
        clear_composition();
    }

    void handle_input_changed(const std::string &before)
    {
        message.clear();
        if (!chinese) return;
        const std::string after = keyboard->text();
        if (after.size() <= before.size()) {
            update_composition();
            return;
        }
        const std::string added = after.substr(before.size());
        if (added == " ") {
            keyboard->set_text(before);
            if (!engine.composition().empty()) commit_candidate(0);
            else append_committed(" ");
            return;
        }
        bool pinyin = true;
        for (unsigned char c : added) {
            if (!std::isalpha(c) && c != '\'') pinyin = false;
        }
        if (pinyin) update_composition();
        else {
            keyboard->set_text(before);
            append_committed(added);
        }
    }

    void switch_language()
    {
        if (chinese) {
            if (!active_composition().empty()) commit_candidate(0);
            keyboard->set_text(committed);
            chinese = false;
        } else {
            committed = keyboard->text();
            keyboard->set_text("");
            engine.set_composition("");
            chinese = true;
            if (nine_keyboard) nine_keyboard->set_page(NineKeyKeyboard::Page::Pinyin);
        }
        keyboard->set_language_toggle(false);
        keyboard->set_input_visible(false);
    }

    void switch_layout()
    {
        if (!chinese) return;
        if (nine_key) {
            const std::string pinyin = nine_engine.composition();
            nine_engine.clear();
            keyboard->set_text(pinyin);
            engine.set_composition(pinyin);
            nine_key = false;
        } else {
            const std::string pinyin = engine.composition();
            keyboard->set_text("");
            engine.set_composition("");
            nine_engine.set_digits(ime::Pinyin9Engine::digits_for(pinyin));
            nine_key = true;
        }
        if (nine_keyboard) nine_keyboard->set_page(NineKeyKeyboard::Page::Pinyin);
        save_nine_key_preference(nine_key);
    }

    void append_direct_text(const std::string &text)
    {
        if (!active_composition().empty()) commit_candidate(0);
        append_committed(text);
    }

    bool backspace_hit(int x, int y) const
    {
        constexpr int widths[] = {110, 110, 210, 160};
        const int total = widths[0] + widths[1] + widths[2] + widths[3] +
                          layout.key_gap * 3;
        int key_x = (M5.Display.width() - total) / 2;
        for (int i = 0; i < 3; ++i) key_x += widths[i] + layout.key_gap;
        return y >= layout.special_keys_y &&
               y < layout.special_keys_y + layout.key_height &&
               x >= key_x && x < key_x + widths[3];
    }

    bool candidate_tap(int x, int y)
    {
        if (y < candidate_y() || y >= candidate_y() + candidate_h()) return false;
        if (y < candidate_controls_y()) {
            if (!chinese || !nine_key || nine_engine.interpretation_count() == 0) return true;
            const int width = M5.Display.width();
            const size_t page_start =
                (nine_engine.interpretation() / PINYIN_SLOTS) * PINYIN_SLOTS;
            const int choices_x = PINYIN_DIGITS_W + PINYIN_PAGE_KEY_W;
            const int choices_w = width - PINYIN_DIGITS_W - PINYIN_PAGE_KEY_W * 2;
            const int slot_w = std::max(1, choices_w / PINYIN_SLOTS);
            if (x >= PINYIN_DIGITS_W && x < choices_x) {
                if (page_start >= PINYIN_SLOTS) {
                    nine_engine.select_interpretation(page_start - PINYIN_SLOTS);
                    draw();
                }
            } else if (x >= width - PINYIN_PAGE_KEY_W) {
                if (page_start + PINYIN_SLOTS < nine_engine.interpretation_count()) {
                    nine_engine.select_interpretation(page_start + PINYIN_SLOTS);
                    draw();
                }
            } else if (x >= choices_x && x < width - PINYIN_PAGE_KEY_W) {
                const size_t slot = std::min<size_t>(
                    PINYIN_SLOTS - 1, static_cast<size_t>((x - choices_x) / slot_w));
                const size_t index = page_start + slot;
                if (index < nine_engine.interpretation_count()) {
                    nine_engine.select_interpretation(index);
                    draw();
                }
            }
            return true;
        }
        const int width = M5.Display.width();
        if ((!chinese || !nine_key) &&
            x >= MODE_KEY_X && x < MODE_KEY_X + MODE_KEY_W) {
            switch_language();
            draw();
            return true;
        }
        if (!chinese) return true;
        const int layout_x = MODE_KEY_X + MODE_KEY_W + MODE_KEY_GAP;
        if (!nine_key && x >= layout_x && x < layout_x + LAYOUT_KEY_W) {
            switch_layout();
            draw();
            return true;
        }
        const int candidate_x = nine_key ? MODE_KEY_X
                                         : layout_x + LAYOUT_KEY_W + MODE_KEY_GAP;
        if (x >= candidate_x && x < candidate_x + PAGE_KEY_W) {
            if (active_previous_page()) draw();
            return true;
        }
        if (x >= width - PAGE_KEY_W) {
            if (active_next_page()) draw();
            return true;
        }
        const int slot_w = (width - candidate_x - PAGE_KEY_W * 2) /
                           CANDIDATE_SLOTS;
        const int relative = x - candidate_x - PAGE_KEY_W;
        if (relative < 0) return true;
        const int slot = std::min(CANDIDATE_SLOTS - 1,
                                  relative / std::max(1, slot_w));
        if (!active_candidate(slot).empty()) {
            commit_candidate(static_cast<size_t>(slot));
            draw();
        }
        return true;
    }

    bool handle_nine_key_tap(int x, int y)
    {
        if (!nine_keyboard) return false;
        NineKeyAction action = nine_keyboard->handle_tap(x, y, M5.Display.width());
        if (action.type == NineKeyActionType::None) return false;
        message.clear();
        switch (action.type) {
            case NineKeyActionType::Digit:
                if (action.digit == '1') {
                    append_direct_text("1");
                } else if (action.digit == '0' && nine_engine.digits().empty()) {
                    append_direct_text("0");
                } else if (!nine_engine.append_digit(action.digit)) {
                    message = "Pinyin is too long";
                }
                break;
            case NineKeyActionType::Text:
                append_direct_text(action.text);
                break;
            case NineKeyActionType::Delete:
                if (nine_keyboard->page() == NineKeyKeyboard::Page::Pinyin &&
                    nine_engine.erase_last()) {
                    break;
                }
                utf8::erase_last(&committed);
                break;
            case NineKeyActionType::Reset:
                nine_engine.clear();
                nine_keyboard->set_page(NineKeyKeyboard::Page::Pinyin);
                break;
            case NineKeyActionType::Symbols:
                if (!active_composition().empty()) commit_candidate(0);
                nine_keyboard->set_page(
                    nine_keyboard->page() == NineKeyKeyboard::Page::Symbols
                        ? NineKeyKeyboard::Page::Pinyin : NineKeyKeyboard::Page::Symbols);
                break;
            case NineKeyActionType::Numeric:
                if (!active_composition().empty()) commit_candidate(0);
                nine_keyboard->set_page(
                    nine_keyboard->page() == NineKeyKeyboard::Page::Numeric
                        ? NineKeyKeyboard::Page::Pinyin : NineKeyKeyboard::Page::Numeric);
                break;
            case NineKeyActionType::Space:
                if (!active_composition().empty()) commit_candidate(0);
                else append_committed(" ");
                break;
            case NineKeyActionType::Language:
                switch_language();
                break;
            case NineKeyActionType::Layout:
                switch_layout();
                break;
            default:
                break;
        }
        draw();
        return true;
    }

    bool button_tap(int x, int y, bool *submit) const
    {
        if (y < buttons_y() || y >= buttons_y() + buttons_h()) return false;
        const int dialog_x = 18;
        const int dialog_w = M5.Display.width() - dialog_x * 2;
        const int gap = 16;
        const int button_w = (dialog_w - 36 - gap) / 2;
        const int left_x = dialog_x + 18;
        const int right_x = left_x + button_w + gap;
        if (x >= left_x && x < left_x + button_w) {
            *submit = false;
            return true;
        }
        if (x >= right_x && x < right_x + button_w) {
            *submit = true;
            return true;
        }
        return false;
    }

    void draw_editor()
    {
        const int width = M5.Display.width();
        const int dialog_x = 18;
        const int dialog_w = width - dialog_x * 2;
        M5.Display.fillRoundRect(dialog_x, dialog_y(), dialog_w, dialog_h(),
                                 8, TFT_WHITE);
        M5.Display.drawRoundRect(dialog_x, dialog_y(), dialog_w, dialog_h(),
                                 8, TFT_BLACK);
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(options.title, dialog_w - 36),
                                 width / 2, dialog_y() + 9, TFT_BLACK);

        const int edit_x = dialog_x + 18;
        const int edit_w = dialog_w - 36;
        M5.Display.drawRoundRect(edit_x, editor_y(), edit_w, editor_h(),
                                 6, TFT_BLACK);
        std::string editable = chinese ? committed : keyboard->text();
        if (options.secret && !editable.empty()) {
            editable.assign(utf8::length(editable), '*');
        }
        const int text_x = edit_x + 12;
        const int text_y = editor_y() +
            std::max(0, (editor_h() - system_font::line_height()) / 2);
        const int suffix_w = system_font::measure(options.fixed_suffix);
        const int editable_w = std::max(0, edit_w - 28 - suffix_w);
        while (!editable.empty() && system_font::measure(editable) > editable_w) {
            size_t bytes = 1;
            const unsigned char lead = static_cast<unsigned char>(editable[0]);
            if ((lead & 0xE0) == 0xC0) bytes = 2;
            else if ((lead & 0xF0) == 0xE0) bytes = 3;
            else if ((lead & 0xF8) == 0xF0) bytes = 4;
            editable.erase(0, std::min(bytes, editable.size()));
        }
        system_font::draw_left(M5.Display, editable, text_x, text_y, TFT_BLACK);
        const int caret_x = text_x + system_font::measure(editable) + 3;
        M5.Display.fillRect(caret_x, editor_y() + 10, 3,
                            std::min(editor_h() - 20, system_font::line_height()),
                            TFT_BLACK);
        if (!options.fixed_suffix.empty()) {
            system_font::draw_left(M5.Display, options.fixed_suffix,
                                   caret_x + 7, text_y, TFT_BLACK);
        }

        const int gap = 16;
        const int button_w = (dialog_w - 36 - gap) / 2;
        const int left_x = dialog_x + 18;
        const int right_x = left_x + button_w + gap;
        M5.Display.drawRoundRect(left_x, buttons_y(), button_w, buttons_h(),
                                 6, TFT_BLACK);
        M5.Display.fillRoundRect(right_x, buttons_y(), button_w, buttons_h(),
                                 6, TFT_BLACK);
        system_font::draw_center(M5.Display, "Cancel",
                                 left_x + button_w / 2, buttons_y() + 8,
                                 TFT_BLACK);
        system_font::draw_center(M5.Display, options.submit_label,
                                 right_x + button_w / 2, buttons_y() + 8,
                                 TFT_WHITE);
    }

    void draw_candidates()
    {
        const int width = M5.Display.width();
        const int y = candidate_y();
        M5.Display.fillRect(0, y, width, candidate_h(), TFT_WHITE);
        M5.Display.drawFastHLine(0, y, width, TFT_BLACK);
        M5.Display.drawFastHLine(0, y + candidate_h() - 1, width, TFT_BLACK);
        if (chinese && !nine_key && !active_composition().empty()) {
            std::string composition = active_composition();
            system_font::draw_center(
                M5.Display,
                system_font::ellipsize(composition, width - 40),
                width / 2, y + 15, TFT_BLACK);
        } else if (chinese && nine_key && !nine_engine.digits().empty()) {
            const int row_y = y + 10;
            const int row_h = candidate_controls_y() - y - 20;
            const int choices_x = PINYIN_DIGITS_W + PINYIN_PAGE_KEY_W;
            const int choices_w = width - PINYIN_DIGITS_W - PINYIN_PAGE_KEY_W * 2;
            const int slot_w = std::max(1, choices_w / PINYIN_SLOTS);
            const size_t page_start =
                (nine_engine.interpretation() / PINYIN_SLOTS) * PINYIN_SLOTS;

            system_font::draw_center(M5.Display, nine_engine.digits(),
                                     PINYIN_DIGITS_W / 2, row_y, TFT_BLACK);
            system_font::draw_center(M5.Display, "<",
                                     PINYIN_DIGITS_W + PINYIN_PAGE_KEY_W / 2,
                                     row_y, TFT_BLACK);
            system_font::draw_center(M5.Display, ">",
                                     width - PINYIN_PAGE_KEY_W / 2,
                                     row_y, TFT_BLACK);
            for (int slot = 0; slot < PINYIN_SLOTS; ++slot) {
                const size_t index = page_start + static_cast<size_t>(slot);
                const std::string &value = nine_engine.interpretation_at(index);
                if (value.empty()) continue;
                const int slot_x = choices_x + slot * slot_w;
                system_font::draw_center(
                    M5.Display, system_font::ellipsize(value, slot_w - 12),
                    slot_x + slot_w / 2, row_y, TFT_BLACK);
                if (index == nine_engine.interpretation()) {
                    M5.Display.fillRect(slot_x + 10, row_y + row_h - 3,
                                        std::max(1, slot_w - 20), 2, TFT_BLACK);
                }
            }
        }

        const int controls_y = candidate_controls_y();
        const int controls_h = candidate_controls_h();
        if (!chinese || !nine_key) {
            M5.Display.drawRoundRect(MODE_KEY_X, controls_y + 2, MODE_KEY_W,
                                     controls_h - 4, 5, TFT_BLACK);
            system_font::draw_center(M5.Display, chinese ? "中/EN" : "EN/中",
                                     MODE_KEY_X + MODE_KEY_W / 2, controls_y + 2,
                                     TFT_BLACK);
        }
        if (!chinese) return;
        const int layout_x = MODE_KEY_X + MODE_KEY_W + MODE_KEY_GAP;
        if (!nine_key) {
            M5.Display.drawRoundRect(layout_x, controls_y + 2, LAYOUT_KEY_W,
                                     controls_h - 4, 5, TFT_BLACK);
            system_font::draw_center(M5.Display, "9键",
                                     layout_x + LAYOUT_KEY_W / 2, controls_y + 2,
                                     TFT_BLACK);
        }
        const int candidate_x = nine_key ? MODE_KEY_X
                                         : layout_x + LAYOUT_KEY_W + MODE_KEY_GAP;
        system_font::draw_center(M5.Display, "<",
                                 candidate_x + PAGE_KEY_W / 2, controls_y + 2,
                                 TFT_BLACK);
        system_font::draw_center(M5.Display, ">",
                                 width - PAGE_KEY_W / 2, controls_y + 2, TFT_BLACK);
        const int slot_w = (width - candidate_x - PAGE_KEY_W * 2) /
                           CANDIDATE_SLOTS;
        for (int i = 0; i < CANDIDATE_SLOTS; ++i) {
            std::string value = active_candidate(i);
            while (!value.empty() &&
                   (utf8::length(value) > 4 ||
                    system_font::measure(value) > slot_w - 8)) {
                utf8::erase_last(&value);
            }
            if (!value.empty()) {
                system_font::draw_center(
                    M5.Display, value,
                    candidate_x + PAGE_KEY_W + i * slot_w + slot_w / 2,
                    controls_y + 2, TFT_BLACK);
            }
        }
    }

    void draw()
    {
        if (!shown || !keyboard) return;
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillRect(0, overlay_top(), M5.Display.width(),
                            M5.Display.height() - overlay_top(), TFT_WHITE);
        M5.Display.drawFastHLine(0, overlay_top(), M5.Display.width(), TFT_BLACK);
        draw_editor();
        draw_candidates();
        if (chinese && nine_key) nine_keyboard->draw(M5.Display);
        else keyboard->draw(M5.Display);
        if (!message.empty()) {
            system_font::draw_center(
                M5.Display, system_font::ellipsize(message, M5.Display.width() - 32),
                M5.Display.width() / 2, candidate_y() - 42, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

TextInputOverlay::TextInputOverlay() : _impl(new Impl) {}
TextInputOverlay::~TextInputOverlay() = default;

void TextInputOverlay::open(const TextInputOptions &options,
                            TextInputCallback on_submit,
                            std::function<void()> on_cancel)
{
    _impl->options = options;
    _impl->submit_callback = std::move(on_submit);
    _impl->cancel_callback = std::move(on_cancel);
    _impl->layout = overlay_keyboard_layout();
    const size_t limit = options.max_bytes > options.fixed_suffix.size()
                           ? options.max_bytes - options.fixed_suffix.size() : 0;
    _impl->keyboard.reset(new VirtualKeyboard(limit, _impl->layout));
    _impl->nine_keyboard.reset(new NineKeyKeyboard(
        _impl->layout.keys_y, M5.Display.height() - 20,
        _impl->layout.horizontal_padding, _impl->layout.key_gap));
    _impl->keyboard->set_language_toggle(false);
    _impl->keyboard->set_input_visible(false);
    _impl->keyboard->set_submit_visible(false);
    _impl->keyboard->set_secret(options.secret);
    _impl->committed = utf8::truncate_bytes(options.initial_text, limit);
    _impl->message.clear();
    _impl->engine.set_composition("");
    _impl->nine_engine.clear();
    _impl->chinese = options.start_in_chinese;
    _impl->nine_key = load_nine_key_preference();
    if (!_impl->chinese) _impl->keyboard->set_text(_impl->committed);
    _impl->shown = true;
    _impl->draw();
}

bool TextInputOverlay::visible() const { return _impl->shown; }

void TextInputOverlay::draw() { _impl->draw(); }

void TextInputOverlay::cancel()
{
    if (!_impl->shown) return;
    _impl->shown = false;
    auto callback = std::move(_impl->cancel_callback);
    _impl->submit_callback = {};
    _impl->keyboard.reset();
    _impl->nine_keyboard.reset();
    _impl->engine.set_composition("");
    _impl->nine_engine.clear();
    _impl->committed.clear();
    if (callback) callback();
}

bool TextInputOverlay::handle_touch(const TouchEvent &event)
{
    if (!_impl->shown) return false;
    if (event.type == TouchEventType::Back) {
        cancel();
        return true;
    }
    if (event.type != TouchEventType::Tap) return true;

    bool submit_button = false;
    if (_impl->button_tap(event.x, event.y, &submit_button)) {
        if (!submit_button) {
            cancel();
        } else {
            if (_impl->chinese && !_impl->active_composition().empty()) {
                _impl->commit_candidate(0);
            } else if (!_impl->chinese) {
                _impl->committed = _impl->keyboard->text();
            }
            const std::string result = _impl->committed + _impl->options.fixed_suffix;
            _impl->shown = false;
            auto callback = std::move(_impl->submit_callback);
            _impl->cancel_callback = {};
            _impl->keyboard.reset();
            _impl->nine_keyboard.reset();
            _impl->engine.set_composition("");
            _impl->nine_engine.clear();
            _impl->committed.clear();
            if (callback) callback(result);
        }
        return true;
    }
    if (_impl->candidate_tap(event.x, event.y)) return true;
    if (_impl->chinese && _impl->nine_key &&
        _impl->handle_nine_key_tap(event.x, event.y)) return true;

    const std::string before = _impl->keyboard->text();
    switch (_impl->keyboard->handle_tap(event.x, event.y, M5.Display.width())) {
        case VirtualKeyboardAction::InputChanged:
            _impl->handle_input_changed(before);
            _impl->draw();
            return true;
        case VirtualKeyboardAction::LayoutChanged:
            _impl->draw();
            return true;
        case VirtualKeyboardAction::LimitReached:
            _impl->message = "Text is too long";
            _impl->draw();
            return true;
        case VirtualKeyboardAction::None:
            if (_impl->chinese && before.empty() &&
                _impl->backspace_hit(event.x, event.y)) {
                utf8::erase_last(&_impl->committed);
                _impl->draw();
            }
            return true;
        default:
            return true;
    }
}

bool TextInputOverlay::handle_key(int key)
{
    if (!_impl->shown) return false;
    if (key == APP_KEY_BACK) {
        cancel();
    } else if (key == APP_KEY_UP && _impl->chinese) {
        if (_impl->active_previous_page()) _impl->draw();
    } else if (key == APP_KEY_DOWN && _impl->chinese) {
        if (_impl->active_next_page()) _impl->draw();
    } else if (key == APP_KEY_EXTRA) {
        if (_impl->chinese && _impl->nine_key && _impl->nine_engine.erase_last()) {
            // Numeric composition changed in place.
        } else if (_impl->chinese && !_impl->keyboard->empty()) {
            _impl->keyboard->erase_last();
            _impl->update_composition();
        } else if (_impl->chinese) {
            utf8::erase_last(&_impl->committed);
        } else {
            _impl->keyboard->erase_last();
        }
        _impl->draw();
    } else if (key == APP_KEY_OK) {
        if (_impl->chinese && !_impl->active_composition().empty()) {
            _impl->commit_candidate(0);
        } else if (!_impl->chinese) {
            _impl->committed = _impl->keyboard->text();
        }
        const std::string result = _impl->committed + _impl->options.fixed_suffix;
        _impl->shown = false;
        auto callback = std::move(_impl->submit_callback);
        _impl->cancel_callback = {};
        _impl->keyboard.reset();
        _impl->nine_keyboard.reset();
        _impl->engine.set_composition("");
        _impl->nine_engine.clear();
        _impl->committed.clear();
        if (callback) callback(result);
    }
    return true;
}

}  // namespace ui
