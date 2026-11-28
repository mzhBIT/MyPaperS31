#include "activities/ClockActivity.h"

#include "activities/ActivityManager.h"
#include "activities/StatusBarActivity.h"
#include "clock/PomodoroTimer.h"
#include "ui/UiLocale.h"

#if CONFIG_IDF_TARGET_ESP32S31
#include "boards/MyPaperS31/StandbyS31.h"
#endif

#include <esp_timer.h>
#include <algorithm>
#include <array>

namespace {

using ui_locale::TextId;
using ui_locale::text;
using pomodoro::Phase;
using pomodoro::State;
using pomodoro::second_us;

constexpr int header = app_status_bar::height;
constexpr int inset = 48;
constexpr int digit_width = 32;
constexpr int colon_width = 12;
constexpr int digit_height = 48;

pomodoro::Timer timer;

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct Layout {
    int width = M5.Display.width();
    int content = M5.Display.height() - header;
    int scale = std::max(1, std::min((width - 2 * inset) / (digit_width*4 + colon_width), content / 260));
    int dial_x = (width - (digit_width*4 + colon_width)*scale) / 2;
    int dial_y = at(29);

    int at(int percent) const { return header + content * percent / 100; }

    Rect button(int index) const {
        constexpr int gap = 18;
        const int available = width - 2*inset - gap;
        const int primary = available * 3 / 5;
        const int height = std::min(92, content * 14 / 100);
        return index == 0 ? Rect{inset, at(78), primary, height}
                          : Rect{inset + primary + gap, at(78), available - primary, height};
    }
};

TextId phase_label(Phase phase)
{
    switch (phase) {
        case Phase::Focus: return TextId::PomodoroFocus;
        case Phase::ShortBreak: return TextId::PomodoroShortBreak;
        case Phase::LongBreak: return TextId::PomodoroLongBreak;
    }
    return TextId::PomodoroFocus;
}

void centered(TextId id, int center_x, int center_y, int width, int px,
              uint16_t foreground = TFT_BLACK, uint16_t background = TFT_WHITE)
{
    const auto label = app_status_bar::fit(text(id), width, px);
    system_font::draw_on_background(M5.Display, label,
        center_x - system_font::measure(label, px) / 2,
        system_font::centered_y(label, center_y, px), px, foreground, background);
}

class ClockActivity final : public StatusBarActivity {
public:
    void on_create() override
    {
        const auto now = esp_timer_get_time();
        timer.update(now);
        draw_page(now);
    }

    void on_show() override
    {
        if (_shown) _dirty = true;
        _shown = true;
    }

    void on_standby_resume() override { _dirty = true; }

    void loop() override
    {
        const auto now = esp_timer_get_time();
        if (timer.update(now)) _dirty = true;
#if CONFIG_IDF_TARGET_ESP32S31
        // A visible running countdown is active use, like audio playback.
        // Manual standby still goes through the board's normal power path.
        if (timer.running() && now - _last_active >= second_us) {
            mypaper::board::s31::standby::note_user_activity();
            _last_active = now;
        }
#endif
        const int seconds = timer.remaining_seconds(now);
        if (!M5.Display.displayBusy() && !touch_input_needs_fast_poll()) {
            if (_dirty) draw_page(now);
            else if (seconds != _last_seconds) draw_tick(seconds);
        }
        StatusBarActivity::loop();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) return on_key(APP_KEY_BACK);
        if (status_bar_touch(event)) return true;
        if (event.type != TouchEventType::Tap) return false;
        const Layout layout;
        for (int index = 0; index < 2; ++index) {
            if (!layout.button(index).contains(event.x, event.y)) continue;
            _focus = -1;
            activate(index);
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        switch (key) {
            case APP_KEY_BACK:
                ActivityManager::instance().pop();
                return true;
            case APP_KEY_OK:
                activate(_focus == 1 ? 1 : 0);
                return true;
            case APP_KEY_UP:
                return on_navigation_key(-1);
            case APP_KEY_DOWN:
                return on_navigation_key(1);
            default:
                return false;
        }
    }

    bool on_navigation_key(int direction) override
    {
        _focus = _focus < 0 ? (direction > 0 ? 0 : 1) : 1 - _focus;
        _dirty = true;
        return true;
    }

private:
    bool _shown = false, _dirty = true;
    int _focus = -1, _last_seconds = -1;
#if CONFIG_IDF_TARGET_ESP32S31
    int64_t _last_active = -second_us;
#endif
    std::array<char, 5> _digits{};

    void activate(int index)
    {
        const auto now = esp_timer_get_time();
        if (index == 0) timer.toggle(now);
        else timer.reset(now);
        _dirty = true;
    }

    // Digits have fixed advances. Preserve every cell that did not change.
    Rect draw_digits(const Layout &layout, int seconds, bool full)
    {
        auto &display = M5.Display;
        const int minutes = seconds / 60;
        const int remainder = seconds % 60;
        const std::array<char, 5> value{char('0' + minutes/10), char('0' + minutes%10), ':',
                                        char('0' + remainder/10), char('0' + remainder%10)};
        const auto style = display.getTextStyle();
        display.setTextSize(layout.scale);
        display.setTextDatum(lgfx::datum_t::top_left);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        const int height = digit_height * layout.scale;
        const int y = layout.dial_y;
        int x = layout.dial_x;
        Rect changed{0, y, 0, height};
        for (size_t index = 0; index < value.size(); ++index) {
            const int width = (index == 2 ? colon_width : digit_width) * layout.scale;
            if (full || _digits[index] != value[index]) {
                display.fillRect(x, y, width, height, TFT_WHITE);
                const char glyph[] = {value[index], '\0'};
                display.drawString(glyph, x, y, &lgfx::fonts::Font7);
                if (!changed.w) changed.x = x;
                changed.w = x + width - changed.x;
                _digits[index] = value[index];
            }
            x += width;
        }
        display.setTextStyle(style);
        _last_seconds = seconds;
        return changed;
    }

    void draw_tick(int seconds)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        const auto changed = draw_digits(Layout{}, seconds, false);
        if (changed.w) M5.Display.display(changed.x, changed.y, changed.w, changed.h);
        M5.Display.endWrite();
    }

    void draw_page(int64_t now)
    {
        const Layout layout;
        const int mid = layout.width / 2;
        const int px = system_font::pixel_size();
        auto &display = M5.Display;
        display.startWrite();
        display.setEpdMode(epd_mode_t::epd_text);
        display.fillScreen(TFT_WHITE);
        draw_header(text(TextId::PomodoroClock));
        centered(phase_label(timer.phase()), mid, layout.at(20), layout.width-2*inset, std::min(px+8, 48));
        draw_digits(layout, timer.remaining_seconds(now), true);

        if (const auto completed = timer.completed())
            centered(*completed == Phase::Focus ? TextId::PomodoroFocusDone : TextId::PomodoroBreakDone,
                     mid, layout.at(50), layout.width-2*inset, px);
        else if (timer.state() == State::Paused)
            centered(TextId::Paused, mid, layout.at(50), layout.width-2*inset, px);

        centered(TextId::PomodoroRounds, mid, layout.at(58), layout.width-2*inset, px);
        const int dot_y = layout.at(64);
        constexpr int dot_gap = 44;
        for (int round = 0; round < pomodoro::cycle_size; ++round) {
            const int x = mid + (2*round - pomodoro::cycle_size + 1) * dot_gap / 2;
            if (round < timer.rounds()) display.fillCircle(x, dot_y, 10, TFT_BLACK);
            else {
                display.drawCircle(x, dot_y, 10, TFT_BLACK);
                display.drawCircle(x, dot_y, 9, TFT_BLACK);
            }
        }

        const TextId primary = timer.running() ? TextId::PomodoroPause :
            timer.state() == State::Paused ? TextId::PomodoroResume : TextId::PomodoroStart;
        const TextId labels[] = {primary, TextId::PomodoroReset};
        for (int index = 0; index < 2; ++index) {
            const auto box = layout.button(index);
            const bool filled = index == 0;
            if (filled) display.fillRoundRect(box.x, box.y, box.w, box.h, 12, TFT_BLACK);
            else display.drawRoundRect(box.x, box.y, box.w, box.h, 12, TFT_BLACK);
            centered(labels[index], box.x + box.w/2, box.y + box.h/2, box.w-24, px,
                     filled ? TFT_WHITE : TFT_BLACK, filled ? TFT_BLACK : TFT_WHITE);
            if (_focus == index)
                display.fillRect(box.x + box.w/2 - 18, box.y + box.h + 8, 36, 4, TFT_BLACK);
        }
        display.display();
        display.endWrite();
        _dirty = false;
    }
};

} // namespace

void launch_clock() { ActivityManager::instance().push(new ClockActivity()); }
