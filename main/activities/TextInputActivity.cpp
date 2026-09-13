#include "activities/TextInputActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ime/PinyinEngine.h"
#include "ui/SystemFont.h"
#include "ui/Utf8Text.h"
#include "ui/VirtualKeyboard.h"

#include <M5Unified.h>
#include <algorithm>
#include <cctype>
#include <utility>

namespace {

constexpr int HEADER_H = 76;
constexpr int EDIT_Y = 92;
constexpr int EDIT_H = 68;
constexpr int CANDIDATE_Y = 258;
constexpr int CANDIDATE_H = 58;
// Three candidates leave enough width for two- and three-character Chinese
// words at the fixed 36 px system-font size. Seven narrow slots collapsed
// almost every phrase to the same "..." label.
constexpr int CANDIDATE_SLOTS = 3;
constexpr int MODE_KEY_X = 24;
constexpr int MODE_KEY_W = 116;
constexpr int MODE_KEY_GAP = 8;
constexpr int PAGE_KEY_W = 36;
constexpr int SECRET_TOGGLE_W = 104;

class TextInputActivity final : public Activity {
public:
    TextInputActivity(TextInputOptions options, TextInputCallback callback)
        : _options(std::move(options)), _callback(std::move(callback)),
          _keyboard(_options.max_bytes > _options.fixed_suffix.size()
                        ? _options.max_bytes - _options.fixed_suffix.size() : 0),
          _engine(CANDIDATE_SLOTS),
          _committed(ui::utf8::truncate_bytes(_options.initial_text, _options.max_bytes)),
          _chinese(_options.start_in_chinese)
    {
        _keyboard.set_secret(false);
        _keyboard.set_submit_label(_options.submit_label);
        // Language switching lives in the candidate row so the keyboard's
        // first special key remains available for Shift/capital letters.
        _keyboard.set_language_toggle(false);
        _keyboard.set_placeholder("");
        _keyboard.set_input_visible(_chinese);
        _keyboard.set_secret(_options.secret);
        if (!_chinese) _keyboard.set_text(_committed);
    }

    void on_create() override { redraw(); }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y < HEADER_H && event.x < 130) {
            ActivityManager::instance().pop();
            return true;
        }
        if (secret_toggle_hit(event.x, event.y)) {
            _secret_visible = !_secret_visible;
            redraw();
            return true;
        }
        // The language key lives in the candidate row and must remain
        // reachable in both modes; otherwise switching to English disables
        // the only path back to Chinese.
        if (candidate_tap(event.x, event.y)) return true;

        std::string before = _keyboard.text();
        ui::VirtualKeyboardAction action = _keyboard.handle_tap(event.x, event.y, M5.Display.width());
        switch (action) {
            case ui::VirtualKeyboardAction::InputChanged:
                handle_input_changed(before);
                redraw();
                return true;
            case ui::VirtualKeyboardAction::LayoutChanged:
                redraw();
                return true;
            case ui::VirtualKeyboardAction::LanguageChanged:
                return true;
            case ui::VirtualKeyboardAction::Submit:
                submit();
                return true;
            case ui::VirtualKeyboardAction::LimitReached:
                _message = "Text is too long";
                redraw();
                return true;
            case ui::VirtualKeyboardAction::None:
                if (_chinese && before.empty() && backspace_hit(event.x, event.y)) {
                    ui::utf8::erase_last(&_committed);
                    redraw();
                    return true;
                }
                return false;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP && _chinese) {
            if (_engine.previous_page()) redraw();
            return true;
        }
        if (key == APP_KEY_DOWN && _chinese) {
            if (_engine.next_page()) redraw();
            return true;
        }
        if (key == APP_KEY_EXTRA) {
            if (_chinese && !_keyboard.empty()) {
                _keyboard.erase_last();
                update_composition();
            } else if (_chinese) {
                ui::utf8::erase_last(&_committed);
            } else {
                _keyboard.erase_last();
            }
            redraw();
            return true;
        }
        if (key == APP_KEY_OK) {
            submit();
            return true;
        }
        return false;
    }

private:
    TextInputOptions _options;
    TextInputCallback _callback;
    ui::VirtualKeyboard _keyboard;
    ime::PinyinEngine _engine;
    std::string _committed;
    std::string _message;
    bool _chinese;
    bool _secret_visible = false;

    size_t committed_limit() const
    {
        return _options.max_bytes > _options.fixed_suffix.size()
                   ? _options.max_bytes - _options.fixed_suffix.size() : 0;
    }

    void update_composition()
    {
        _engine.set_composition(_keyboard.text());
        _keyboard.set_text(_engine.composition());
    }

    void handle_input_changed(const std::string &before)
    {
        _message.clear();
        if (!_chinese) return;
        const std::string after = _keyboard.text();
        if (after.size() <= before.size()) {
            update_composition();
            return;
        }
        std::string added = after.substr(before.size());
        if (added == " ") {
            _keyboard.set_text(before);
            if (!_engine.composition().empty()) commit_candidate(0);
            else append_committed(" ");
            return;
        }
        bool pinyin = true;
        for (unsigned char c : added) {
            if (!std::isalpha(c) && c != '\'') pinyin = false;
        }
        if (pinyin) update_composition();
        else {
            _keyboard.set_text(before);
            append_committed(added);
        }
    }

    void append_committed(const std::string &text)
    {
        if (!ui::utf8::append(&_committed, text, committed_limit())) _message = "Text is too long";
    }

    void commit_candidate(size_t slot)
    {
        std::string candidate = _engine.candidate_on_page(slot);
        if (candidate.empty()) candidate = _engine.composition();
        if (!candidate.empty()) append_committed(candidate);
        _keyboard.set_text("");
        _engine.set_composition("");
    }

    void switch_language()
    {
        if (_chinese) {
            if (!_engine.composition().empty()) commit_candidate(0);
            _keyboard.set_text(_committed);
            _chinese = false;
        } else {
            _committed = _keyboard.text();
            _keyboard.set_text("");
            _engine.set_composition("");
            _chinese = true;
        }
        _keyboard.set_language_toggle(false);
        _keyboard.set_placeholder("");
        _keyboard.set_input_visible(_chinese);
    }

    bool backspace_hit(int x, int y) const
    {
        constexpr int key_y = 706;
        constexpr int key_h = 78;
        constexpr int gap = 8;
        const int widths[] = {110, 110, 210, 160};
        int total = widths[0] + widths[1] + widths[2] + widths[3] + gap * 3;
        int key_x = (M5.Display.width() - total) / 2;
        for (int i = 0; i < 3; ++i) key_x += widths[i] + gap;
        return y >= key_y && y < key_y + key_h && x >= key_x && x < key_x + widths[3];
    }

    bool secret_toggle_hit(int x, int y) const
    {
        if (!_options.secret || y < EDIT_Y || y >= EDIT_Y + EDIT_H) return false;
        const int right = M5.Display.width() - 20;
        return x >= right - SECRET_TOGGLE_W && x < right;
    }

    bool candidate_tap(int x, int y)
    {
        if (y < CANDIDATE_Y || y >= CANDIDATE_Y + CANDIDATE_H) return false;
        int width = M5.Display.width();
        if (x >= MODE_KEY_X && x < MODE_KEY_X + MODE_KEY_W) {
            switch_language();
            redraw();
            return true;
        }
        if (!_chinese) return true;
        const int candidate_x = MODE_KEY_X + MODE_KEY_W + MODE_KEY_GAP;
        if (x >= candidate_x && x < candidate_x + PAGE_KEY_W) {
            if (_engine.previous_page()) redraw();
            return true;
        }
        if (x >= width - PAGE_KEY_W) {
            if (_engine.next_page()) redraw();
            return true;
        }
        int slot_w = (width - candidate_x - PAGE_KEY_W * 2) / CANDIDATE_SLOTS;
        int slot = std::min(CANDIDATE_SLOTS - 1,
                            (x - candidate_x - PAGE_KEY_W) / std::max(1, slot_w));
        if (!_engine.candidate_on_page(slot).empty()) {
            commit_candidate(slot);
            redraw();
        }
        return true;
    }

    void submit()
    {
        if (_chinese) {
            if (!_engine.composition().empty()) commit_candidate(0);
        } else {
            _committed = _keyboard.text();
        }
        std::string result = _committed + _options.fixed_suffix;
        if (_callback) _callback(result);
        ActivityManager::instance().pop();
    }

    void draw_header()
    {
        int width = M5.Display.width();
        M5.Display.fillRect(0, 0, width, HEADER_H, TFT_WHITE);
        system_font::draw_left(M5.Display, "<", 18, 12, TFT_BLACK);
        system_font::draw_center(M5.Display, _options.title, width / 2, 12, TFT_BLACK);
        M5.Display.drawFastHLine(0, HEADER_H - 1, width, TFT_BLACK);
    }

    void draw_editor()
    {
        int width = M5.Display.width();
        int pad = 20;
        M5.Display.drawRoundRect(pad, EDIT_Y, width - pad * 2, EDIT_H, 7, TFT_BLACK);
        std::string editable = _chinese ? _committed : _keyboard.text();
        if (_options.secret && !_secret_visible && !editable.empty()) {
            editable.assign(ui::utf8::length(editable), '*');
        }
        const int text_x = pad + 14;
        const int text_y = EDIT_Y + std::max(0, (EDIT_H - system_font::line_height()) / 2);
        const int inner_w = width - pad * 2 - 28;
        const int suffix_w = system_font::measure(_options.fixed_suffix);
        const int caret_gap = 8;
        const int toggle_space = _options.secret ? SECRET_TOGGLE_W + 10 : 0;
        const int editable_w = std::max(0, inner_w - suffix_w - caret_gap - toggle_space);

        // Keep the insertion point visible for long names by retaining the
        // tail nearest the cursor. No placeholder is drawn for an empty name.
        while (!editable.empty() && system_font::measure(editable) > editable_w) {
            size_t bytes = 1;
            unsigned char lead = static_cast<unsigned char>(editable[0]);
            if ((lead & 0xE0) == 0xC0) bytes = 2;
            else if ((lead & 0xF0) == 0xE0) bytes = 3;
            else if ((lead & 0xF8) == 0xF0) bytes = 4;
            editable.erase(0, std::min(bytes, editable.size()));
        }

        system_font::draw_left(M5.Display, editable, text_x, text_y, TFT_BLACK);
        int caret_x = text_x + system_font::measure(editable) + 3;
        int caret_y = EDIT_Y + 12;
        int caret_h = std::min(EDIT_H - 24, system_font::line_height());
        M5.Display.fillRect(caret_x, caret_y, 3, caret_h, TFT_BLACK);
        if (!_options.fixed_suffix.empty()) {
            system_font::draw_left(M5.Display, _options.fixed_suffix,
                                   caret_x + caret_gap, text_y, TFT_BLACK);
        }
        if (_options.secret) {
            const int toggle_x = width - pad - SECRET_TOGGLE_W;
            M5.Display.fillRect(toggle_x - 6, EDIT_Y + 1, SECRET_TOGGLE_W + 5,
                                EDIT_H - 2, TFT_WHITE);
            M5.Display.drawFastVLine(toggle_x - 6, EDIT_Y + 8, EDIT_H - 16, TFT_BLACK);
            system_font::draw_center(M5.Display, _secret_visible ? "Hide" : "Show",
                                     toggle_x + SECRET_TOGGLE_W / 2,
                                     EDIT_Y + std::max(0, (EDIT_H - system_font::line_height()) / 2),
                                     TFT_BLACK);
        }
    }

    void draw_candidates()
    {
        int width = M5.Display.width();
        M5.Display.fillRect(0, CANDIDATE_Y, width, CANDIDATE_H, TFT_WHITE);
        M5.Display.drawFastHLine(0, CANDIDATE_Y + CANDIDATE_H - 1, width, TFT_BLACK);
        M5.Display.drawRoundRect(MODE_KEY_X, CANDIDATE_Y + 3,
                                 MODE_KEY_W, CANDIDATE_H - 7, 5, TFT_BLACK);
        system_font::draw_center(M5.Display, _chinese ? "中/EN" : "EN/中",
                                 MODE_KEY_X + MODE_KEY_W / 2,
                                 CANDIDATE_Y + 7, TFT_BLACK);
        if (!_chinese) return;
        const int candidate_x = MODE_KEY_X + MODE_KEY_W + MODE_KEY_GAP;
        system_font::draw_center(M5.Display, "<", candidate_x + PAGE_KEY_W / 2,
                                 CANDIDATE_Y + 7, TFT_BLACK);
        system_font::draw_center(M5.Display, ">", width - PAGE_KEY_W / 2,
                                 CANDIDATE_Y + 7, TFT_BLACK);
        int slot_w = (width - candidate_x - PAGE_KEY_W * 2) / CANDIDATE_SLOTS;
        for (int i = 0; i < CANDIDATE_SLOTS; ++i) {
            std::string value = _engine.candidate_on_page(i);
            if (!value.empty()) {
                // Candidate labels intentionally omit an ellipsis: with the
                // large Chinese font it can consume the complete slot and
                // make every candidate indistinguishable.
                while (!value.empty() &&
                       (ui::utf8::length(value) > 4 ||
                        system_font::measure(value) > slot_w - 8)) {
                    ui::utf8::erase_last(&value);
                }
                system_font::draw_center(M5.Display, value,
                                         candidate_x + PAGE_KEY_W + i * slot_w + slot_w / 2,
                                         CANDIDATE_Y + 7, TFT_BLACK);
            }
        }
    }

    void redraw()
    {
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();
        draw_editor();
        _keyboard.draw(M5.Display);
        draw_candidates();
        if (!_message.empty()) {
            system_font::draw_center(M5.Display, _message, M5.Display.width() / 2, 962, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

}  // namespace

void launch_text_input(const TextInputOptions &options, TextInputCallback on_submit)
{
    ActivityManager::instance().push(new TextInputActivity(options, std::move(on_submit)));
}
