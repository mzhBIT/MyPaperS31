#include "activities/DeepSeekImagePickerActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"

#include <M5Unified.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

constexpr const char *ROOT = "/sdcard";
constexpr int HEADER_H = header_layout::height;
constexpr int ROW_H = 82;
constexpr int PAD = 20;

struct Entry { std::string name; bool directory = false; };

bool is_supported_image(const char *name)
{
    const char *ext = std::strrchr(name, '.');
    return ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 ||
                   strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".gif") == 0 ||
                   strcasecmp(ext, ".webp") == 0);
}

class DeepSeekImagePickerActivity final : public Activity {
public:
    explicit DeepSeekImagePickerActivity(DeepSeekImagePickCallback callback)
        : _callback(std::move(callback)) {}

    void on_create() override { load(ROOT); draw(true); }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y, 110))) {
            back();
            return true;
        }
        if (event.type == TouchEventType::SwipeUp) { move(visible_rows()); return true; }
        if (event.type == TouchEventType::SwipeDown) { move(-visible_rows()); return true; }
        if (event.type != TouchEventType::Tap || event.y < HEADER_H) return false;
        const int index = _scroll + (event.y - HEADER_H) / ROW_H;
        if (index >= 0 && index < static_cast<int>(_entries.size())) activate(index);
        return true;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) back();
        else if (key == APP_KEY_UP) move(-1);
        else if (key == APP_KEY_DOWN) move(1);
        else if (key == APP_KEY_OK && !_entries.empty()) activate(_focus);
        return true;
    }

private:
    DeepSeekImagePickCallback _callback;
    std::string _path = ROOT;
    std::vector<Entry> _entries;
    int _focus = 0;
    int _scroll = 0;

    int visible_rows() const
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
            std::string full = _path + "/" + entry->d_name;
            struct stat st = {};
            if (stat(full.c_str(), &st) != 0) continue;
            const bool directory = S_ISDIR(st.st_mode);
            if (directory || is_supported_image(entry->d_name)) {
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
        const size_t slash = _path.find_last_of('/');
        load(slash == std::string::npos || slash <= std::strlen(ROOT) ? ROOT
                                                                       : _path.substr(0, slash));
        draw(false);
    }

    void move(int delta)
    {
        if (_entries.empty()) return;
        _focus = std::clamp(_focus + delta, 0, static_cast<int>(_entries.size()) - 1);
        const int rows = visible_rows();
        if (_focus < _scroll) _scroll = _focus;
        if (_focus >= _scroll + rows) _scroll = _focus - rows + 1;
        draw(false);
    }

    void activate(int index)
    {
        const Entry &entry = _entries[static_cast<size_t>(index)];
        const std::string full = _path + "/" + entry.name;
        if (entry.directory) { load(full); draw(false); return; }
        if (_callback) _callback(full);
        ActivityManager::instance().pop();
    }

    void draw(bool quality)
    {
        const int width = M5.Display.width();
        const int rows = visible_rows();
        _scroll = std::clamp(_scroll, 0, std::max(0, static_cast<int>(_entries.size()) - rows));
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        system_font::draw_left(M5.Display, "<", 18, header_layout::text_y(), TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ChooseImage), width / 2, header_layout::text_y(), TFT_BLACK);
        header_layout::draw_separator(M5.Display);
        for (int row = 0; row < rows; ++row) {
            const int index = _scroll + row;
            if (index >= static_cast<int>(_entries.size())) break;
            const int y = HEADER_H + row * ROW_H;
            const bool selected = index == _focus;
            const uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
            const uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRect(PAD, y, width - PAD * 2, ROW_H - 3, bg);
            const Entry &entry = _entries[static_cast<size_t>(index)];
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(entry.name + (entry.directory ? "/" : ""),
                                                          width - PAD * 2 - 20),
                                   PAD + 10, y + 16, fg);
            M5.Display.drawFastHLine(PAD, y + ROW_H - 3, width - PAD * 2, TFT_BLACK);
        }
        if (_entries.empty()) {
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::NoDeepSeekImages), width / 2,
                                     180, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

}  // namespace

void launch_deepseek_image_picker(DeepSeekImagePickCallback callback)
{
    ActivityManager::instance().push(new DeepSeekImagePickerActivity(std::move(callback)));
}
