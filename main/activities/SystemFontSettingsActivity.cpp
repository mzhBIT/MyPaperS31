#include "activities/SystemFontSettingsActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/SystemFont.h"
#include "ui/EmbeddedFont.h"
#include "ui/HeaderLayout.h"
#include "ui/ListFocus.h"
#include "ui/UiLocale.h"
#include <algorithm>

namespace {
constexpr int TOP = header_layout::height;
constexpr int LIST_TOP = TOP + 250;
constexpr int ROW_H = 88;
constexpr int PAD = 20;
class SystemFontSettingsActivity final : public Activity {
public:
    void on_create() override {
        _fonts = system_font::available_fonts();
        auto current = std::find(_fonts.begin(), _fonts.end(), system_font::font_path());
        if (current != _fonts.end()) _focus = current - _fonts.begin();
        scroll_to_focus(); draw(true);
    }
    bool on_touch(const TouchEvent &event) override {
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y))) {
            ActivityManager::instance().pop(); return true;
        }
        if (event.type == TouchEventType::SwipeUp) { move(visible()); return true; }
        if (event.type == TouchEventType::SwipeDown) { move(-visible()); return true; }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y >= TOP && event.y < TOP + 100) {
            if (event.x < 180) resize(-2);
            else if (event.x >= M5.Display.width() - 180) resize(2);
            return true;
        }
        const int row = (event.y - LIST_TOP) / ROW_H;
        if (event.y >= LIST_TOP && row < visible() && _scroll + row < (int)_fonts.size()) {
            _focus = _scroll + row; choose(); return true;
        }
        return false;
    }
    bool on_key(int key) override {
        if (key == APP_KEY_BACK) { ActivityManager::instance().pop(); return true; }
        if (key == APP_KEY_UP) move(-1);
        else if (key == APP_KEY_DOWN) move(1);
        else if (key == APP_KEY_OK) choose();
        return true;
    }
private:
    std::vector<std::string> _fonts;
    int _focus = 0, _scroll = 0;
    bool _failed = false;
    int visible() const { return std::max(1, (int(M5.Display.height()) - LIST_TOP - 60) / ROW_H); }
    void scroll_to_focus() {
        if (_focus < _scroll) _scroll = _focus;
        if (_focus >= _scroll + visible()) _scroll = _focus - visible() + 1;
    }
    void move(int delta) {
        _focus = std::max(0, std::min((int)_fonts.size()-1, _focus + delta));
        scroll_to_focus(); draw(false);
    }
    void resize(int delta) {
        _failed = system_font::set_size(std::max(system_font::min_size,
            std::min(system_font::max_size, system_font::pixel_size() + delta))) != ESP_OK;
        draw(true);
    }
    void choose() {
        if (_fonts.empty()) return;
        _failed = system_font::set_font(_fonts[_focus]) != ESP_OK;
        draw(true);
    }
    void draw(bool quality) {
        using ui_locale::TextId;
        using ui_locale::text;
        auto &d = M5.Display;
        d.startWrite(); d.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
        d.fillScreen(TFT_WHITE);
        system_font::draw_left(d, "<", PAD, header_layout::text_y());
        system_font::draw_center(d, text(TextId::SystemFont), d.width()/2, header_layout::text_y());
        header_layout::draw_separator(d);
        const int center = TOP + 32;
        system_font::draw_left(d, "-", 65, center);
        system_font::draw_center(d, std::string(text(TextId::FontSize)) + ": " +
                                 std::to_string(system_font::pixel_size()) + " px", d.width()/2, center);
        system_font::draw_right(d, "+", d.width()-65, center);
        d.drawFastHLine(PAD, TOP+99, d.width()-2*PAD, TFT_BLACK);
        system_font::draw_left(d, system_font::ellipsize(text(TextId::SystemSizeHint), d.width()-2*PAD), PAD, TOP+110);
        system_font::draw_left(d, "Aa 123  简体中文  繁體中文", PAD, TOP+164);
        for (int row=0; row<visible(); ++row) {
            const int i = _scroll + row;
            if (i >= (int)_fonts.size()) break;
            const int top = LIST_TOP + row*ROW_H;
            list_focus::draw(d, PAD, top, ROW_H, i == _focus);
            const std::string &path = _fonts[i];
            const std::string name = path == embedded_font::path ? text(TextId::EmbeddedUiFont)
                : path.substr(path.find_last_of('/') + 1);
            system_font::draw_left(d, system_font::ellipsize(name, d.width()-2*PAD-105),
                                   PAD+list_focus::text_inset(), top+20);
            if (path == system_font::font_path()) system_font::draw_right(d, "*", d.width()-PAD-16, top+20);
            d.drawFastHLine(PAD, top+ROW_H-1, d.width()-2*PAD, TFT_BLACK);
        }
        system_font::draw_left(d, system_font::ellipsize(text(_failed ? TextId::SystemFontFailed : TextId::SystemFontHint),
                                d.width()-2*PAD), PAD, d.height()-52);
        d.display(); d.endWrite();
    }
};
}
void launch_system_font_settings() {
    ActivityManager::instance().push(new SystemFontSettingsActivity());
}
