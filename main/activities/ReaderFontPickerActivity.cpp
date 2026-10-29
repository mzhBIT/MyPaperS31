#include "activities/Activity.h"
#include "ui/ListFocus.h"
#include "activities/ActivityManager.h"
#include "reader_font.h"
#include "reader_style.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/AppStatusBar.h"
#include "ui/UiLocale.h"

#include <M5Unified.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

static const int TOP_H = header_layout::height;
static const int ROW_H = 72;
static const int PAD = 18;

static const char *base_name(const char *path)
{
    if (!path || !path[0]) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool is_font_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && (strcasecmp(ext, ".ttf") == 0 ||
                   strcasecmp(ext, ".otf") == 0 ||
                   strcasecmp(ext, ".bin") == 0);
}

static void draw_header(const char *title)
{
    int w = M5.Display.width();
    M5.Display.fillRect(0, 0, w, TOP_H, TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    system_font::draw_center(M5.Display, title, w / 2, header_layout::text_y(), TFT_BLACK);
    header_layout::draw_separator(M5.Display);
}

class FontPickerActivity : public Activity {
    ReaderProfile m_profile = ReaderProfile::Shared;
public:
    explicit FontPickerActivity(ReaderProfile profile = ReaderProfile::Shared) : m_profile(profile) {}
private:
    app_status_bar::State m_status_bar;
    int top_height()const {return m_profile==ReaderProfile::Rss ? app_status_bar::height : TOP_H;}
    std::vector<std::string> m_fonts;
    int m_focus = 0;
    int m_scroll = 0;

    void load_fonts()
    {
        m_fonts.clear();
        DIR *dir = opendir("/sdcard/fonts");
        if (!dir) return;

        struct dirent *ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (!is_font_file(ent->d_name)) continue;
            std::string path = std::string("/sdcard/fonts/") + ent->d_name;
            struct stat st = {};
            if (stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
                m_fonts.push_back(path);
            }
        }
        closedir(dir);
        std::sort(m_fonts.begin(), m_fonts.end());
    }

    void focus_current_font()
    {
        const char *current = reader_font_path(m_profile);
        if (!current || !current[0] || m_fonts.empty()) return;

        int found = -1;
        for (int i = 0; i < (int)m_fonts.size(); ++i) {
            if (m_fonts[i] == current) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            const char *current_name = base_name(current);
            for (int i = 0; i < (int)m_fonts.size(); ++i) {
                if (strcmp(base_name(m_fonts[i].c_str()), current_name) == 0) {
                    found = i;
                    break;
                }
            }
        }
        if (found < 0) return;

        m_focus = found;
        int rows = std::max(1, ((int)M5.Display.height() - top_height() - 20) / ROW_H);
        m_scroll = std::max(0, m_focus - rows / 2);
    }

    void redraw()
    {
        int w = M5.Display.width();
        int h = M5.Display.height();
        int y0 = top_height();
        int rows = std::max(1, (h - y0 - 12) / ROW_H);
        int max_scroll = std::max(0, (int)m_fonts.size() - rows);
        m_scroll = std::min(std::max(0, m_scroll), max_scroll);

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        if(m_profile==ReaderProfile::Rss)m_status_bar.draw(ui_locale::text(ui_locale::TextId::Fonts));
        else draw_header(ui_locale::text(ui_locale::TextId::Fonts));

        if (m_fonts.empty()) {
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::NoFontsHint), w / 2, y0 + 80);
        }

        for (int i = 0; i < rows; ++i) {
            int idx = m_scroll + i;
            if (idx >= (int)m_fonts.size()) break;
            int y = y0 + i * ROW_H;
            bool sel = idx == m_focus;
            list_focus::draw(M5.Display, PAD, y, ROW_H, sel);
            M5.Display.drawFastHLine(PAD, y + ROW_H - 1, w - PAD * 2, TFT_BLACK);
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            std::string label = system_font::ellipsize(base_name(m_fonts[idx].c_str()), w - PAD * 2 - list_focus::text_inset() - 14);
            system_font::draw_left(M5.Display, label, PAD + list_focus::text_inset(), y + 12, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }

    int visible_rows() const
    {
        int h = M5.Display.height();
        return std::max(1, (h - top_height() - 20) / ROW_H);
    }

    int hit_row(int y) const
    {
        int y0 = top_height();
        if (y < y0) return -1;
        int idx = m_scroll + (y - y0) / ROW_H;
        return idx >= 0 && idx < (int)m_fonts.size() ? idx : -1;
    }

    void move_focus(int delta)
    {
        if (m_fonts.empty()) return;
        int rows = visible_rows();
        m_focus = std::max(0, std::min((int)m_fonts.size() - 1, m_focus + delta));
        if (m_focus < m_scroll) m_scroll = m_focus;
        if (m_focus >= m_scroll + rows) m_scroll = m_focus - rows + 1;
        redraw();
    }

    void select_focused()
    {
        if (m_fonts.empty()) return;
        reader_font_set(m_fonts[m_focus].c_str(), m_profile);
        ActivityManager::instance().pop();
    }

public:
    void on_create() override
    {
        load_fonts();
        focus_current_font();
        redraw();
    }
    void loop() override {
        if(m_profile==ReaderProfile::Rss)m_status_bar.refresh(ui_locale::text(ui_locale::TextId::Fonts));
    }

    bool on_key(int key) override
    {
        int rows = visible_rows();

        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (m_fonts.empty()) return true;

        if (key == APP_KEY_UP) {
            if (m_focus > 0) m_focus--;
            if (m_focus < m_scroll) m_scroll = m_focus;
            redraw();
            return true;
        }
        if (key == APP_KEY_DOWN) {
            if (m_focus + 1 < (int)m_fonts.size()) m_focus++;
            if (m_focus >= m_scroll + rows) m_scroll = m_focus - rows + 1;
            redraw();
            return true;
        }
        if (key == APP_KEY_OK) {
            reader_font_set(m_fonts[m_focus].c_str(), m_profile);
            ActivityManager::instance().pop();
            return true;
        }
        return true;
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (m_fonts.empty()) return true;
        int rows = visible_rows();
        if (event.type == TouchEventType::SwipeUp) {
            move_focus(rows);
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            move_focus(-rows);
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;

        int idx = hit_row(event.y);
        if (idx < 0) return false;
        if (idx != m_focus) {
            m_focus = idx;
            if (m_focus < m_scroll) m_scroll = m_focus;
            if (m_focus >= m_scroll + rows) m_scroll = m_focus - rows + 1;
            redraw();
            return true;
        }
        select_focused();
        return true;
    }
};

void launch_reader_font_picker()
{
    ActivityManager::instance().push(new FontPickerActivity());
}

void launch_reader_font_picker(ReaderProfile profile)
{
    ActivityManager::instance().push(new FontPickerActivity(profile));
}
