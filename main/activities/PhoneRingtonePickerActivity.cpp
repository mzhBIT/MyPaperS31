#include "activities/PhoneRingtonePickerActivity.h"
#include "ui/ListFocus.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "audio/audio_file_formats.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"

#include <M5Unified.h>
#include <algorithm>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

constexpr char ROOT[] = "/sdcard";
constexpr int HEADER_H = header_layout::height;
constexpr int ROW_H = 82;
constexpr int PAD = 20;

struct Entry { std::string name; bool directory; };

class PhoneRingtonePickerActivity final : public Activity {
public:
    explicit PhoneRingtonePickerActivity(PhoneRingtonePickCallback callback)
        : _callback(std::move(callback)) {}

    void on_create() override { load(ROOT); }
    void on_show() override { draw(); }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y, 110))) {
            back(); return true;
        }
        if (event.type == TouchEventType::SwipeUp) { move(rows()); return true; }
        if (event.type == TouchEventType::SwipeDown) { move(-rows()); return true; }
        if (event.type != TouchEventType::Tap || event.y < HEADER_H) return false;
        const int index = _scroll + (event.y - HEADER_H) / ROW_H;
        if (index >= 0 && index < static_cast<int>(_entries.size())) select(index);
        return true;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) back();
        else if (key == APP_KEY_UP) move(-1);
        else if (key == APP_KEY_DOWN) move(1);
        else if (key == APP_KEY_OK && !_entries.empty()) select(_focus);
        return true;
    }

private:
    PhoneRingtonePickCallback _callback;
    std::string _path = ROOT;
    std::vector<Entry> _entries;
    int _focus = 0;
    int _scroll = 0;

    int rows() const
    {
        return std::max(1, static_cast<int>((M5.Display.height() - HEADER_H - 12) / ROW_H));
    }

    void load(const std::string &path)
    {
        _path = path;
        _entries.clear();
        _focus = _scroll = 0;
        DIR *dir = opendir(path.c_str());
        if (!dir) return;
        for (dirent *entry = readdir(dir); entry; entry = readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            const std::string full = _path + "/" + entry->d_name;
            struct stat st{};
            if (stat(full.c_str(), &st) != 0) continue;
            const bool directory = S_ISDIR(st.st_mode);
            if (directory || (S_ISREG(st.st_mode) &&
                              audio_file_formats::is_supported(entry->d_name))) {
                _entries.push_back({entry->d_name, directory});
            }
        }
        closedir(dir);
        std::sort(_entries.begin(), _entries.end(), [](const Entry &a, const Entry &b) {
            if (a.directory != b.directory) return a.directory > b.directory;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    }

    void back()
    {
        if (_path == ROOT) { ActivityManager::instance().pop(); return; }
        const auto slash = _path.find_last_of('/');
        load(slash <= sizeof(ROOT) - 1 ? ROOT : _path.substr(0, slash));
        draw();
    }

    void move(int delta)
    {
        if (_entries.empty()) return;
        _focus = std::clamp(_focus + delta, 0, static_cast<int>(_entries.size()) - 1);
        if (_focus < _scroll) _scroll = _focus;
        if (_focus >= _scroll + rows()) _scroll = _focus - rows() + 1;
        draw();
    }

    void select(int index)
    {
        const auto &entry = _entries[static_cast<size_t>(index)];
        const std::string full = _path + "/" + entry.name;
        if (entry.directory) { load(full); draw(); return; }
        if (_callback) _callback(full);
        ActivityManager::instance().pop();
    }

    void draw()
    {
        const int width = M5.Display.width();
        _scroll = std::clamp(_scroll, 0,
            std::max(0, static_cast<int>(_entries.size()) - rows()));
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        system_font::draw_left(M5.Display, "<", PAD, header_layout::text_y(), TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ChooseRingtone), width / 2, header_layout::text_y(), TFT_BLACK);
        header_layout::draw_separator(M5.Display);
        for (int row = 0; row < rows(); ++row) {
            const int index = _scroll + row;
            if (index >= static_cast<int>(_entries.size())) break;
            const int y = HEADER_H + row * ROW_H;
            const auto &entry = _entries[static_cast<size_t>(index)];
            list_focus::draw(M5.Display, PAD, y, ROW_H, index == _focus);
            system_font::draw_left(M5.Display,
                system_font::ellipsize(entry.name + (entry.directory ? "/" : ""),
                                       width - PAD * 2 - list_focus::text_inset() - 10),
                PAD + list_focus::text_inset(), y + 16, TFT_BLACK);
            M5.Display.drawFastHLine(PAD, y + ROW_H - 3, width - PAD * 2, TFT_DARKGRAY);
        }
        if (_entries.empty()) system_font::draw_center(M5.Display,
            ui_locale::text(ui_locale::TextId::NoAudioFiles), width / 2, 180, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

} // namespace

void launch_phone_ringtone_picker(PhoneRingtonePickCallback callback)
{
    ActivityManager::instance().push(new PhoneRingtonePickerActivity(std::move(callback)));
}
