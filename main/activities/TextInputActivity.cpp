#include "activities/TextInputActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/TextInputOverlay.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/AppStatusBar.h"
#include <M5Unified.h>
#include <utility>

namespace {

// Activity-based callers share the same editor/IME as file rename and search.
// Keep the Activity boundary for existing submit/pop and RSS two-step flows.
class TextInputActivity final : public Activity {
public:
    TextInputActivity(TextInputOptions options, TextInputCallback callback)
        : _options(std::move(options)), _callback(std::move(callback)) {}

    void on_create() override
    {
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(_options.text_refresh ? epd_mode_t::epd_text : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        if (_options.unified_status_bar) _status_bar.draw(_options.title.c_str());
        else {
            system_font::draw_left(M5.Display,"<",18,header_layout::text_y(),TFT_BLACK);
            system_font::draw_center(M5.Display,_options.title,M5.Display.width()/2,
                                    header_layout::text_y(),TFT_BLACK);
            header_layout::draw_separator(M5.Display);
        }
        _editor.open(_options,[this](const std::string &result) {
            _result=result; _submitted=true; _closing=true;
        },[this]() { _closing=true; });
        M5.Display.endWrite();
    }

    void loop() override
    {
        if (_closing) {
            // Pop outside the overlay's event handler so its callback cannot
            // destroy the keyboard/Activity while that handler is still active.
            if (_submitted && _callback) _callback(_result);
            ActivityManager::instance().pop();
            return;
        }
        if (_options.unified_status_bar) _status_bar.refresh(_options.title.c_str());
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (_closing) return true;
        if (!_options.unified_status_bar && event.type==TouchEventType::Tap &&
            header_layout::left_hit(event.x,event.y,130)) {
            _editor.cancel();
            return true;
        }
        return _editor.handle_touch(event);
    }

    bool on_key(int key) override
    {
        return _closing || _editor.handle_key(key);
    }

private:
    TextInputOptions _options;
    TextInputCallback _callback;
    ui::TextInputOverlay _editor;
    app_status_bar::State _status_bar;
    std::string _result;
    bool _submitted=false, _closing=false;
};

} // namespace

void launch_text_input(const TextInputOptions &options, TextInputCallback on_submit)
{
    ActivityManager::instance().push(new TextInputActivity(options,std::move(on_submit)));
}
