#include "activities/ReaderBackgroundPickerActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/SystemFont.h"
#include "ui/reader_background.h"
#include "ui/reader_book_cover.h"

#include <M5Unified.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr int TOP_H = 56;
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
    explicit ReaderBackgroundPickerActivity(bool book_cover = false)
        : m_book_cover(book_cover)
    {
    }

    void on_create() override
    {
        std::string initial = ROOT;
        const bool enabled = m_book_cover ? reader_book_cover_enabled()
                                          : reader_background_enabled();
        if (enabled) {
            initial = m_book_cover ? reader_book_cover_path()
                                   : reader_background_path();
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
        if (event.y < TOP_H && event.x < 120) {
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
    bool m_book_cover = false;
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
        int top = TOP_H + 6;
        if (y < top) return -1;
        int index = m_scroll + (y - top) / ROW_H;
        return index >= 0 && index < item_count() ? index : -1;
    }

    void activate(int index)
    {
        m_notice.clear();
        if (index == 0) {
            const bool cleared = m_book_cover ? reader_book_cover_clear()
                                              : reader_background_clear();
            if (cleared) ActivityManager::instance().pop();
            else {
                m_notice = m_book_cover ? "Failed to disable book cover"
                                        : "Failed to disable background";
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
        const bool selected = m_book_cover
            ? reader_book_cover_set(full.c_str(), &m_notice)
            : reader_background_set(full.c_str(), M5.Display.width(), M5.Display.height(),
                                    &m_notice) == ReaderBackgroundResult::Ok;
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
        system_font::draw_left(M5.Display, "<", 16, 7, TFT_BLACK);
        system_font::draw_center(M5.Display,
                                 m_book_cover ? "Lock-screen Book Cover" : "Reading Background",
                                 width / 2, 7, TFT_BLACK);
        M5.Display.drawFastHLine(0, TOP_H - 1, width, TFT_BLACK);

        int top = TOP_H + 6;
        for (int row = 0; row < rows; ++row) {
            int index = m_scroll + row;
            if (index >= item_count()) break;
            int y = top + row * ROW_H;
            bool selected = index == m_focus;
            uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
            uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRect(PAD, y, width - PAD * 2, ROW_H - 2, bg);
            M5.Display.drawFastHLine(PAD, y + ROW_H - 2, width - PAD * 2, TFT_BLACK);
            std::string label;
            if (index == 0) label = m_book_cover ? "Off" : "Off (white background)";
            else {
                const Entry &entry = m_entries[(size_t)index - 1];
                label = entry.name + (entry.directory ? "/" : "");
            }
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(label, width - PAD * 2 - 28),
                                   PAD + 12, y + 13, fg);
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

void launch_reader_background_picker()
{
    ActivityManager::instance().push(new ReaderBackgroundPickerActivity());
}

void launch_reader_book_cover_picker()
{
    ActivityManager::instance().push(new ReaderBackgroundPickerActivity(true));
}
