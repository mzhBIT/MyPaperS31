#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "reader_font.h"
#include "reader_style.h"
#include "ui/SystemFont.h"

#include <M5Unified.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

static const int TOP_H = 56;
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
    system_font::draw_center(M5.Display, title, w / 2, 6, TFT_BLACK);
    M5.Display.drawFastHLine(0, TOP_H - 1, w, TFT_BLACK);
}

class FontPickerActivity : public Activity {
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
        const char *current = reader_font_path();
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
        int rows = std::max(1, ((int)M5.Display.height() - TOP_H - 20) / ROW_H);
        m_scroll = std::max(0, m_focus - rows / 2);
    }

    void redraw()
    {
        int w = M5.Display.width();
        int h = M5.Display.height();
        int y0 = TOP_H + 8;
        int rows = std::max(1, (h - y0 - 12) / ROW_H);
        int max_scroll = std::max(0, (int)m_fonts.size() - rows);
        m_scroll = std::min(std::max(0, m_scroll), max_scroll);

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("Fonts");

        if (m_fonts.empty()) {
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            system_font::draw_center(M5.Display, "No fonts in /sdcard/fonts", w / 2, y0 + 80);
        }

        for (int i = 0; i < rows; ++i) {
            int idx = m_scroll + i;
            if (idx >= (int)m_fonts.size()) break;
            int y = y0 + i * ROW_H;
            bool sel = idx == m_focus;
            M5.Display.fillRoundRect(PAD, y, w - PAD * 2, ROW_H - 6, 6, sel ? TFT_BLACK : TFT_WHITE);
            M5.Display.drawRoundRect(PAD, y, w - PAD * 2, ROW_H - 6, 6, TFT_BLACK);
            M5.Display.setTextColor(sel ? TFT_WHITE : TFT_BLACK, sel ? TFT_BLACK : TFT_WHITE);
            std::string label = system_font::ellipsize(base_name(m_fonts[idx].c_str()), w - PAD * 2 - 28);
            system_font::draw_left(M5.Display, label, PAD + 14, y + 12, sel ? TFT_WHITE : TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }

    int visible_rows() const
    {
        int h = M5.Display.height();
        return std::max(1, (h - TOP_H - 20) / ROW_H);
    }

    int hit_row(int y) const
    {
        int y0 = TOP_H + 8;
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
        reader_font_set(m_fonts[m_focus].c_str());
        ActivityManager::instance().pop();
    }

public:
    void on_create() override
    {
        load_fonts();
        focus_current_font();
        redraw();
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
            reader_font_set(m_fonts[m_focus].c_str());
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

class ReaderSettingsActivity : public Activity {
    int m_focus = 0;

    static const int ROW_COUNT = 9;

    void adjust(int delta)
    {
        ReaderStyle style = reader_style_get();
        switch (m_focus) {
        case 1:
            style.refresh_mode += delta;
            if (style.refresh_mode < 1) style.refresh_mode = 4;
            if (style.refresh_mode > 4) style.refresh_mode = 1;
            break;
        case 2:
            style.font_size += delta * 2;
            break;
        case 3:
            style.line_spacing += delta * 2;
            break;
        case 4:
            style.first_line_indent += delta;
            break;
        case 5:
            style.paragraph_spacing += delta;
            break;
        case 6:
            style.margin_left += delta * 5;
            break;
        case 7:
            style.margin_right += delta * 5;
            break;
        case 8:
            style.show_images = style.show_images ? 0 : 1;
            break;
        default:
            return;
        }
        reader_style_set(style);
        reader_style_save();
    }

    void row_text(int row, char *buf, size_t len)
    {
        ReaderStyle style = reader_style_get();
        switch (row) {
        case 0:
            snprintf(buf, len, "Font: %s", base_name(reader_font_path()));
            break;
        case 1:
            snprintf(buf, len, "Refresh: %s", reader_style_refresh_mode_name(style.refresh_mode));
            break;
        case 2:
            snprintf(buf, len, "Font size: %d", style.font_size);
            break;
        case 3:
            snprintf(buf, len, "Line spacing: %d", style.line_spacing);
            break;
        case 4:
            snprintf(buf, len, "First-line indent: %d", style.first_line_indent);
            break;
        case 5:
            snprintf(buf, len, "Paragraph gap: %.1f", style.paragraph_spacing * 0.1);
            break;
        case 6:
            snprintf(buf, len, "Left margin: %d", style.margin_left);
            break;
        case 7:
            snprintf(buf, len, "Right margin: %d", style.margin_right);
            break;
        case 8:
            snprintf(buf, len, "Show images: %s", style.show_images ? "On" : "Off");
            break;
        default:
            if (len > 0) buf[0] = '\0';
            break;
        }
    }

    void redraw()
    {
        int w = M5.Display.width();
        int y0 = TOP_H + 16;

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("Reader Settings");

        for (int i = 0; i < ROW_COUNT; ++i) {
            int y = y0 + i * ROW_H;
            bool sel = i == m_focus;
            M5.Display.fillRoundRect(PAD, y, w - PAD * 2, ROW_H - 6, 6, sel ? TFT_BLACK : TFT_WHITE);
            M5.Display.drawRoundRect(PAD, y, w - PAD * 2, ROW_H - 6, 6, TFT_BLACK);
            M5.Display.setTextColor(sel ? TFT_WHITE : TFT_BLACK, sel ? TFT_BLACK : TFT_WHITE);
            char text[128];
            row_text(i, text, sizeof(text));
            std::string label = system_font::ellipsize(text, w - PAD * 2 - 28);
            system_font::draw_left(M5.Display, label, PAD + 14, y + 12, sel ? TFT_WHITE : TFT_BLACK);
        }

        M5.Display.display();
        M5.Display.endWrite();
    }

    int hit_row(int y) const
    {
        int y0 = TOP_H + 16;
        if (y < y0) return -1;
        int idx = (y - y0) / ROW_H;
        return idx >= 0 && idx < ROW_COUNT ? idx : -1;
    }

    void activate_focused()
    {
        if (m_focus == 0) {
            ActivityManager::instance().push(new FontPickerActivity());
        } else {
            adjust(1);
            redraw();
        }
    }

public:
    void on_create() override
    {
        redraw();
    }

    void on_show() override
    {
        redraw();
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_EXTRA) {
            reader_style_save();
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP) {
            if (m_focus > 0) m_focus--;
            redraw();
            return true;
        }
        if (key == APP_KEY_DOWN) {
            if (m_focus + 1 < ROW_COUNT) m_focus++;
            redraw();
            return true;
        }
        if (key == APP_KEY_OK) {
            if (m_focus == 0) {
                ActivityManager::instance().push(new FontPickerActivity());
            } else {
                adjust(1);
                redraw();
            }
            return true;
        }
        if (key == APP_KEY_BACK) {
            adjust(-1);
            redraw();
            return true;
        }
        return true;
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            reader_style_save();
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::SwipeUp) {
            if (m_focus + 1 < ROW_COUNT) m_focus++;
            redraw();
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            if (m_focus > 0) m_focus--;
            redraw();
            return true;
        }
        if (event.type == TouchEventType::SwipeLeft) {
            adjust(-1);
            redraw();
            return true;
        }
        if (event.type == TouchEventType::SwipeRight) {
            adjust(1);
            redraw();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;

        int idx = hit_row(event.y);
        if (idx < 0) return false;
        if (idx != m_focus) {
            m_focus = idx;
            redraw();
            return true;
        }
        activate_focused();
        return true;
    }
};

void launch_reader_settings()
{
    ActivityManager::instance().push(new ReaderSettingsActivity());
}

void launch_reader_font_picker()
{
    ActivityManager::instance().push(new FontPickerActivity());
}
