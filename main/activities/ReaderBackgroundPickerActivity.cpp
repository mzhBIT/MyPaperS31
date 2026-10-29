#include "activities/ReaderBackgroundPickerActivity.h"
#include "ui/ListFocus.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"
#include "ui/reader_background.h"
#include "ui/reader_book_cover.h"
#include "ui/standby_wallpaper.h"

#include <M5Unified.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr int TOP_H = header_layout::height;
constexpr int ROW_H = 72;
constexpr int PAD = 18;
constexpr const char *ROOT = "/sdcard";

struct Entry {
    std::string name;
    bool directory = false;
};

bool is_image(const char *name)
{
    const char *ext = std::strrchr(name, '.');
    return ext && (strcasecmp(ext, ".jpg") == 0 ||
                   strcasecmp(ext, ".jpeg") == 0 ||
                   strcasecmp(ext, ".png") == 0 ||
                   strcasecmp(ext, ".bmp") == 0);
}

class ReaderBackgroundPickerActivity final : public Activity {
public:
    explicit ReaderBackgroundPickerActivity(bool book_cover = false, ReaderProfile profile = ReaderProfile::Shared,
                                            bool standby = false)
        : m_profile(profile), m_book_cover(book_cover), m_standby(standby)
    {
    }

    void on_create() override
    {
        std::string initial = ROOT;
        const bool enabled = m_standby ? standby_wallpaper_enabled() : m_book_cover ? reader_book_cover_enabled(m_profile)
                                          : reader_background_enabled(m_profile);
        if (enabled) {
            initial = m_standby ? standby_wallpaper_path() : m_book_cover ? reader_book_cover_path(m_profile)
                                   : reader_background_path(m_profile);
            size_t slash = initial.find_last_of('/');
            initial = slash == std::string::npos || slash == 0
                        ? ROOT : initial.substr(0, slash);
            if (initial.rfind(ROOT, 0) != 0) initial = ROOT;
        }
        load(initial);
        redraw();
    }

    bool on_key(int key) override
    {
        if (!m_notice.empty()) {
            m_notice.clear();
            redraw();
            return true;
        }
        if (key == APP_KEY_BACK) {
            back();
            return true;
        }
        if (key == APP_KEY_UP) move(-1);
        else if (key == APP_KEY_DOWN) move(1);
        else if (key == APP_KEY_OK) activate(m_focus);
        return true;
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (!m_notice.empty()) {
            if (event.type == TouchEventType::Tap || event.type == TouchEventType::Back) {
                m_notice.clear();
                redraw();
            }
            return true;
        }
        if (event.type == TouchEventType::Back) {
            back();
            return true;
        }
        if (event.type == TouchEventType::SwipeUp) {
            move(visible_rows());
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            move(-visible_rows());
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (header_layout::left_hit(event.x, event.y, 120)) {
            back();
            return true;
        }
        int index = hit(event.y);
        if (index < 0) return false;
        m_focus = index;
        activate(index);
        return true;
    }

private:
    ReaderProfile m_profile = ReaderProfile::Shared;
    bool m_book_cover = false;
    bool m_standby = false;
    std::string m_path = ROOT;
    std::vector<Entry> m_entries;
    int m_focus = 0;
    int m_scroll = 0;
    std::string m_notice;

    int item_count() const { return (int)m_entries.size() + 1; }

    int visible_rows() const
    {
        return std::max(1, ((int)M5.Display.height() - TOP_H - 12) / ROW_H);
    }

    void load(const std::string &path)
    {
        m_path = path;
        m_entries.clear();
        m_focus = 0;
        m_scroll = 0;
        DIR *dir = opendir(path.c_str());
        if (!dir) return;
        for (dirent *entry = readdir(dir); entry; entry = readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            std::string full = m_path + "/" + entry->d_name;
            struct stat st = {};
            if (stat(full.c_str(), &st) != 0) continue;
            bool directory = S_ISDIR(st.st_mode);
            if (!directory && !is_image(entry->d_name)) continue;
            m_entries.push_back({entry->d_name, directory});
        }
        closedir(dir);
        std::sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) {
            if (a.directory != b.directory) return a.directory > b.directory;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    }

    void back()
    {
        if (m_path == ROOT) {
            ActivityManager::instance().pop();
            return;
        }
        size_t slash = m_path.find_last_of('/');
        load(slash == std::string::npos || slash < std::strlen(ROOT)
                 ? ROOT : m_path.substr(0, slash));
        redraw();
    }

    void move(int delta)
    {
        m_focus = std::max(0, std::min(item_count() - 1, m_focus + delta));
        int rows = visible_rows();
        if (m_focus < m_scroll) m_scroll = m_focus;
        if (m_focus >= m_scroll + rows) m_scroll = m_focus - rows + 1;
        redraw();
    }

    int hit(int y) const
    {
        int top = TOP_H;
        if (y < top) return -1;
        int index = m_scroll + (y - top) / ROW_H;
        return index >= 0 && index < item_count() ? index : -1;
    }

    void activate(int index)
    {
        m_notice.clear();
        if (index == 0) {
            const bool cleared = m_standby ? standby_wallpaper_clear() : m_book_cover ? reader_book_cover_clear(m_profile)
                                              : reader_background_clear(m_profile);
            if (cleared) ActivityManager::instance().pop();
            else {
                m_notice = ui_locale::text(m_book_cover ? ui_locale::TextId::DisableBookCoverFailed
                                                      : ui_locale::TextId::DisableBackgroundFailed);
                redraw();
            }
            return;
        }
        const Entry &entry = m_entries[(size_t)index - 1];
        std::string full = m_path + "/" + entry.name;
        if (entry.directory) {
            load(full);
            redraw();
            return;
        }
        const bool selected = m_standby
            ? standby_wallpaper_set(full.c_str(), M5.Display.width(), M5.Display.height(), &m_notice) == ReaderBackgroundResult::Ok
            : m_book_cover
            ? reader_book_cover_set(full.c_str(), &m_notice, m_profile)
            : reader_background_set(full.c_str(), M5.Display.width(), M5.Display.height(),
                                    &m_notice, m_profile) == ReaderBackgroundResult::Ok;
        if (selected) {
            ActivityManager::instance().pop();
        } else {
            redraw();
        }
    }

    void redraw()
    {
        int width = M5.Display.width();
        int rows = visible_rows();
        int max_scroll = std::max(0, item_count() - rows);
        m_scroll = std::max(0, std::min(max_scroll, m_scroll));

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        system_font::draw_left(M5.Display, "<", 16, header_layout::text_y(), TFT_BLACK);
        system_font::draw_center(M5.Display,
                                 ui_locale::text(m_standby ? ui_locale::TextId::StandbyWallpaper : m_book_cover ? ui_locale::TextId::LockScreenBookCover
                                                              : ui_locale::TextId::ReadingBackground),
                                 width / 2, header_layout::text_y(), TFT_BLACK);
        header_layout::draw_separator(M5.Display);

        int top = TOP_H;
        for (int row = 0; row < rows; ++row) {
            int index = m_scroll + row;
            if (index >= item_count()) break;
            int y = top + row * ROW_H;
            bool selected = index == m_focus;
            constexpr uint16_t fg = TFT_BLACK;
            list_focus::draw(M5.Display, PAD, y, ROW_H, selected);
            M5.Display.drawFastHLine(PAD, y + ROW_H - 2, width - PAD * 2, TFT_BLACK);
            std::string label;
            if (index == 0) label = ui_locale::text(m_standby ? ui_locale::TextId::StandbyKeepScreen : m_book_cover ? ui_locale::TextId::Off
                                                                  : ui_locale::TextId::OffWhiteBackground);
            else {
                const Entry &entry = m_entries[(size_t)index - 1];
                label = entry.name + (entry.directory ? "/" : "");
            }
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(label, width - PAD * 2 - list_focus::text_inset() - 16),
                                   PAD + list_focus::text_inset(), y + 13, fg);
        }
        if (!m_notice.empty()) {
            int box_width = std::min(width - 48, 820);
            int x = (width - box_width) / 2;
            int y = M5.Display.height() / 2 - 72;
            M5.Display.fillRoundRect(x, y, box_width, 144, 7, TFT_WHITE);
            M5.Display.drawRoundRect(x, y, box_width, 144, 7, TFT_BLACK);
            system_font::draw_center(M5.Display,
                                     system_font::ellipsize(m_notice, box_width - 28),
                                     width / 2, y + 42, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

} // namespace

void launch_reader_background_picker(ReaderProfile profile)
{
    ActivityManager::instance().push(new ReaderBackgroundPickerActivity(false, profile));
}

void launch_reader_book_cover_picker(ReaderProfile profile)
{
    ActivityManager::instance().push(new ReaderBackgroundPickerActivity(true, profile));
}

void launch_standby_wallpaper_picker()
{
    ActivityManager::instance().push(new ReaderBackgroundPickerActivity(false, ReaderProfile::Shared, true));
}
