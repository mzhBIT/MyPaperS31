#include "activities/ReaderOverlay.h"

#include "reader_font.h"
#include "reader_style.h"
#include "ui/SystemFont.h"

#include <M5Unified.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr int PANEL_PAD = 12;
constexpr int STEP_BOX_W = 96;
constexpr int STEP_BOX_GAP = 10;
constexpr int VALUE_W = 100;
constexpr int CHAPTER_INDEX_H = 52;

const char *base_name(const char *path)
{
    if (!path || !path[0]) return "Default";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void setting_text(int index, std::string *name, std::string *value)
{
    ReaderStyle style = reader_style_get();
    char buf[64] = {};
    switch (index) {
    case 0:
        *name = "Font";
        *value = base_name(reader_font_path());
        break;
    case 1:
        *name = "Refresh";
        *value = reader_style_refresh_mode_name(style.refresh_mode);
        break;
    case 2:
        *name = "Font size";
        snprintf(buf, sizeof(buf), "%d", style.font_size);
        *value = buf;
        break;
    case 3:
        *name = "Line spacing";
        snprintf(buf, sizeof(buf), "%d", style.line_spacing);
        *value = buf;
        break;
    case 4:
        *name = "First-line indent";
        snprintf(buf, sizeof(buf), "%d", style.first_line_indent);
        *value = buf;
        break;
    case 5:
        *name = "Paragraph gap";
        snprintf(buf, sizeof(buf), "%.1f", style.paragraph_spacing * 0.1f);
        *value = buf;
        break;
    case 6:
        *name = "Left margin";
        snprintf(buf, sizeof(buf), "%d", style.margin_left);
        *value = buf;
        break;
    case 7:
        *name = "Right margin";
        snprintf(buf, sizeof(buf), "%d", style.margin_right);
        *value = buf;
        break;
    case 8:
        *name = "Show images";
        *value = style.show_images ? "On" : "Off";
        break;
    default:
        name->clear();
        value->clear();
        break;
    }
}

} // namespace

void ReaderOverlay::set_chapters(const ReaderOverlayChapters &chapters)
{
    m_chapters = chapters;
}

void ReaderOverlay::set_chapters_enabled(bool enabled)
{
    m_chapters_enabled = enabled;
    if (!m_chapters_enabled) m_tab = Tab::Settings;
}

ReaderOverlay::Geometry ReaderOverlay::geometry() const
{
    int screen_w = M5.Display.width();
    int screen_h = M5.Display.height();
    Geometry g;
    g.x = screen_w / 100;
    g.y = screen_h * 10 / 100;
    g.w = screen_w * 98 / 100;
    g.h = screen_h * 80 / 100;
    g.tab_h = 60;
    g.body_y = g.y;
    g.tab_y = g.y + g.h - g.tab_h;
    g.body_h = g.tab_y - g.body_y;
    return g;
}

int ReaderOverlay::chapter_count() const
{
    return m_chapters.count ? std::max(0, m_chapters.count(m_chapters.context)) : 0;
}

int ReaderOverlay::visible_chapter_rows() const
{
    Geometry g = geometry();
    return std::max(1, (g.body_h - CHAPTER_INDEX_H) / 66);
}

void ReaderOverlay::clamp_chapter_scroll()
{
    int count = chapter_count();
    if (count <= 0) {
        m_chapter_focus = 0;
        m_chapter_scroll = 0;
        return;
    }
    int rows = visible_chapter_rows();
    m_chapter_focus = std::max(0, std::min(count - 1, m_chapter_focus));
    int max_scroll = std::max(0, count - rows);
    m_chapter_scroll = std::max(0, std::min(max_scroll, m_chapter_scroll));
    if (m_chapter_focus < m_chapter_scroll) m_chapter_scroll = m_chapter_focus;
    if (m_chapter_focus >= m_chapter_scroll + rows) {
        m_chapter_scroll = m_chapter_focus - rows + 1;
    }
}

void ReaderOverlay::select_tab(Tab tab)
{
    if (!m_chapters_enabled && tab == Tab::Chapters) tab = Tab::Settings;
    m_tab = tab;
    if (m_tab != Tab::Chapters) return;
    if (m_chapters.current) {
        m_chapter_focus = m_chapters.current(m_chapters.context);
        m_chapter_scroll = std::max(0, m_chapter_focus - visible_chapter_rows() / 2);
    }
    clamp_chapter_scroll();
}

void ReaderOverlay::open(Tab tab)
{
    m_visible = true;
    select_tab(tab);
}

void ReaderOverlay::close()
{
    m_visible = false;
}

void ReaderOverlay::adjust_setting(int delta)
{
    if (m_setting_focus == 0) return;
    ReaderStyle style = reader_style_get();
    switch (m_setting_focus) {
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
    m_settings_dirty = true;
}

void ReaderOverlay::move_setting_focus(int delta)
{
    m_setting_focus = std::max(0, std::min(SETTING_COUNT - 1, m_setting_focus + delta));
}

void ReaderOverlay::move_chapter_focus(int delta)
{
    int count = chapter_count();
    if (count <= 0) return;
    m_chapter_focus = std::max(0, std::min(count - 1, m_chapter_focus + delta));
    clamp_chapter_scroll();
}

int ReaderOverlay::hit_setting(int x, int y, int *direction) const
{
    if (direction) *direction = 0;
    Geometry g = geometry();
    int inner_x = g.x + PANEL_PAD;
    int inner_w = g.w - PANEL_PAD * 2;
    if (y < g.body_y || y >= g.tab_y) return -1;
    int index = (y - g.body_y) * SETTING_COUNT / g.body_h;
    int cell_x = inner_x;
    int cell_w = inner_w;
    if (index < 0 || index >= SETTING_COUNT || x < cell_x || x >= cell_x + cell_w) return -1;
    if (index > 0 && direction) {
        int plus_x = cell_x + cell_w - STEP_BOX_W;
        int minus_x = plus_x - STEP_BOX_GAP - VALUE_W - STEP_BOX_GAP - STEP_BOX_W;
        if ((index == 1 || index == 8) && x >= minus_x && x < plus_x + STEP_BOX_W) {
            *direction = 1;
        } else if (x >= minus_x && x < minus_x + STEP_BOX_W) *direction = -1;
        else if (x >= plus_x && x < plus_x + STEP_BOX_W) *direction = 1;
    }
    return index;
}

int ReaderOverlay::hit_chapter(int y) const
{
    Geometry g = geometry();
    int list_y = g.body_y + CHAPTER_INDEX_H;
    int list_h = g.body_h - CHAPTER_INDEX_H;
    if (y < list_y || y >= g.tab_y) return -1;
    int index = m_chapter_scroll + (y - list_y) * visible_chapter_rows() / list_h;
    return index >= 0 && index < chapter_count() ? index : -1;
}

ReaderOverlay::Action ReaderOverlay::activate_setting()
{
    if (m_setting_focus == 0) return Action::OpenFont;
    adjust_setting(1);
    redraw();
    return Action::None;
}

ReaderOverlay::Action ReaderOverlay::activate_chapter()
{
    if (!m_chapters.activate || chapter_count() <= 0) return Action::None;
    if (m_chapters.activate(m_chapters.context, m_chapter_focus)) {
        m_visible = false;
        return Action::ChapterOpened;
    }
    return Action::None;
}

ReaderOverlay::Action ReaderOverlay::on_key(int key)
{
    if (!m_visible) return Action::None;
    if (key == APP_KEY_BACK) return Action::Close;
    if (key == APP_KEY_EXTRA) {
        if (!m_chapters_enabled) return Action::None;
        select_tab(m_tab == Tab::Settings ? Tab::Chapters : Tab::Settings);
        redraw();
        return Action::None;
    }
    if (m_tab == Tab::Settings) {
        if (key == APP_KEY_UP) move_setting_focus(-1);
        else if (key == APP_KEY_DOWN) move_setting_focus(1);
        else if (key == APP_KEY_OK) return activate_setting();
        else return Action::None;
    } else {
        if (key == APP_KEY_UP) move_chapter_focus(-1);
        else if (key == APP_KEY_DOWN) move_chapter_focus(1);
        else if (key == APP_KEY_OK) return activate_chapter();
        else return Action::None;
    }
    redraw();
    return Action::None;
}

ReaderOverlay::Action ReaderOverlay::on_touch(const TouchEvent &event)
{
    if (!m_visible) return Action::None;
    Geometry g = geometry();
    if (event.type == TouchEventType::Back) return Action::Close;

    if (event.type == TouchEventType::SwipeLeft || event.type == TouchEventType::SwipeRight) {
        if (m_tab == Tab::Settings) {
            adjust_setting(event.type == TouchEventType::SwipeLeft ? -1 : 1);
        } else {
            select_tab(event.type == TouchEventType::SwipeLeft ? Tab::Chapters : Tab::Settings);
        }
        redraw();
        return Action::None;
    }
    if (event.type == TouchEventType::SwipeUp || event.type == TouchEventType::SwipeDown) {
        int delta = event.type == TouchEventType::SwipeUp ? 1 : -1;
        if (m_tab == Tab::Settings) move_setting_focus(delta);
        else move_chapter_focus(delta * visible_chapter_rows());
        redraw();
        return Action::None;
    }
    if (event.type != TouchEventType::Tap) return Action::None;

    if (event.x < g.x || event.x >= g.x + g.w || event.y < g.y || event.y >= g.y + g.h) {
        return Action::Close;
    }
    if (event.y >= g.tab_y) {
        select_tab(!m_chapters_enabled || event.x < g.x + g.w / 2
                       ? Tab::Settings
                       : Tab::Chapters);
        redraw();
        return Action::None;
    }
    if (event.y < g.body_y) return Action::None;

    if (m_tab == Tab::Settings) {
        int direction = 0;
        int index = hit_setting(event.x, event.y, &direction);
        if (index < 0) return Action::None;
        m_setting_focus = index;
        if (index == 0) return Action::OpenFont;
        if (direction != 0) adjust_setting(direction);
        redraw();
        return Action::None;
    }

    int index = hit_chapter(event.y);
    if (index < 0) return Action::None;
    m_chapter_focus = index;
    return activate_chapter();
}

void ReaderOverlay::draw_settings(const Geometry &g)
{
    int inner_x = g.x + PANEL_PAD;
    int inner_w = g.w - PANEL_PAD * 2;
    for (int index = 0; index < SETTING_COUNT; ++index) {
        int x = inner_x;
        int w = inner_w;
        int row_y0 = g.body_y + index * g.body_h / SETTING_COUNT;
        int row_y1 = g.body_y + (index + 1) * g.body_h / SETTING_COUNT;
        int row_h = row_y1 - row_y0;
        constexpr uint16_t bg = TFT_WHITE;
        constexpr uint16_t fg = TFT_BLACK;
        M5.Display.fillRect(x, row_y0 + 1, w, row_h - 1, bg);
        if (index + 1 < SETTING_COUNT) {
            M5.Display.drawFastHLine(x, row_y1, w, TFT_BLACK);
        }

        std::string name;
        std::string value;
        setting_text(index, &name, &value);
        int text_y = row_y0 + std::max(1, (row_h - system_font::line_height()) / 2);
        system_font::draw_left(M5.Display, name, x + 12, text_y, fg);

        if (index == 0) {
            int max_value_w = std::max(40, w - system_font::measure(name) - 70);
            value = system_font::ellipsize(value, max_value_w);
            system_font::draw_right(M5.Display, value + "  >", x + w - 12, text_y, fg);
        } else {
            int plus_x = x + w - STEP_BOX_W;
            int value_x = plus_x - STEP_BOX_GAP - VALUE_W;
            int minus_x = value_x - STEP_BOX_GAP - STEP_BOX_W;
            int box_y = row_y0 + 6;
            int box_h = std::max(28, row_h - 12);
            int control_text_y = box_y + std::max(0, (box_h - system_font::line_height()) / 2);
            if (index == 1 || index == 8) {
                int toggle_w = plus_x + STEP_BOX_W - minus_x;
                M5.Display.drawRoundRect(minus_x, box_y, toggle_w, box_h, 4, fg);
                value = system_font::ellipsize(value, toggle_w - 16);
                system_font::draw_center(M5.Display, value, minus_x + toggle_w / 2, control_text_y, fg);
            } else {
                M5.Display.drawRoundRect(minus_x, box_y, STEP_BOX_W, box_h, 4, fg);
                M5.Display.drawRoundRect(plus_x, box_y, STEP_BOX_W, box_h, 4, fg);
                system_font::draw_center(M5.Display, "-", minus_x + STEP_BOX_W / 2, control_text_y, fg);
                value = system_font::ellipsize(value, VALUE_W - 8);
                system_font::draw_center(M5.Display, value, value_x + VALUE_W / 2, text_y, fg);
                system_font::draw_center(M5.Display, "+", plus_x + STEP_BOX_W / 2, control_text_y, fg);
            }
        }
    }
}

void ReaderOverlay::draw_chapters(const Geometry &g)
{
    int count = chapter_count();
    if (count <= 0) {
        system_font::draw_center(M5.Display, "No chapter directory", g.x + g.w / 2,
                                 g.body_y + g.body_h / 2 - system_font::line_height() / 2);
        return;
    }

    clamp_chapter_scroll();
    int rows = visible_chapter_rows();
    int list_y = g.body_y + CHAPTER_INDEX_H;
    int list_h = g.body_h - CHAPTER_INDEX_H;
    char position[40] = {};
    snprintf(position, sizeof(position), "Chapter  %d / %d", m_chapter_focus + 1, count);
    int index_text_y = g.body_y + std::max(1, (CHAPTER_INDEX_H - system_font::line_height()) / 2);
    system_font::draw_center(M5.Display, position, g.x + g.w / 2, index_text_y, TFT_BLACK);
    M5.Display.drawFastHLine(g.x + PANEL_PAD, list_y, g.w - PANEL_PAD * 2, TFT_BLACK);

    for (int i = 0; i < rows; ++i) {
        int index = m_chapter_scroll + i;
        if (index >= count) break;
        int row_y0 = list_y + i * list_h / rows;
        int row_y1 = list_y + (i + 1) * list_h / rows;
        int row_h = row_y1 - row_y0;
        bool selected = index == m_chapter_focus;
        uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
        uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
        M5.Display.fillRect(g.x + PANEL_PAD, row_y0 + 1, g.w - PANEL_PAD * 2, row_h - 1, bg);
        if (i + 1 < rows && index + 1 < count) {
            M5.Display.drawFastHLine(g.x + PANEL_PAD, row_y1, g.w - PANEL_PAD * 2, TFT_BLACK);
        }
        int depth = m_chapters.depth ? std::max(0, m_chapters.depth(m_chapters.context, index)) : 0;
        std::string title = m_chapters.title ? m_chapters.title(m_chapters.context, index) : std::string();
        int text_x = g.x + PANEL_PAD + 12 + std::min(depth, 4) * 18;
        title = system_font::ellipsize(title, g.x + g.w - PANEL_PAD - 18 - text_x);
        int text_y = row_y0 + std::max(1, (row_h - system_font::line_height()) / 2);
        system_font::draw_left(M5.Display, title, text_x, text_y, fg);
    }

}

void ReaderOverlay::draw_tabs(const Geometry &g)
{
    if (!m_chapters_enabled) {
        M5.Display.drawFastHLine(g.x, g.tab_y, g.w, TFT_BLACK);
        M5.Display.fillRect(g.x + 1, g.tab_y + 1, g.w - 2, g.tab_h - 2, TFT_BLACK);
        int text_y = g.tab_y + std::max(2, (g.tab_h - system_font::line_height()) / 2);
        system_font::draw_center(M5.Display, "Settings", g.x + g.w / 2, text_y, TFT_WHITE);
        return;
    }

    int half = g.w / 2;
    M5.Display.drawFastHLine(g.x, g.tab_y, g.w, TFT_BLACK);
    M5.Display.drawFastVLine(g.x + half, g.tab_y, g.tab_h, TFT_BLACK);
    bool settings = m_tab == Tab::Settings;
    M5.Display.fillRect(g.x + 1, g.tab_y + 1, half - 1, g.tab_h - 2, settings ? TFT_BLACK : TFT_WHITE);
    M5.Display.fillRect(g.x + half + 1, g.tab_y + 1, g.w - half - 2, g.tab_h - 2,
                        settings ? TFT_WHITE : TFT_BLACK);
    int text_y = g.tab_y + std::max(2, (g.tab_h - system_font::line_height()) / 2);
    system_font::draw_center(M5.Display, "Settings", g.x + half / 2, text_y,
                             settings ? TFT_WHITE : TFT_BLACK);
    system_font::draw_center(M5.Display, "Chapters", g.x + half + (g.w - half) / 2, text_y,
                             settings ? TFT_BLACK : TFT_WHITE);
}

void ReaderOverlay::draw()
{
    if (!m_visible) return;
    Geometry g = geometry();
    M5.Display.fillRoundRect(g.x, g.y, g.w, g.h, 10, TFT_WHITE);
    M5.Display.drawRoundRect(g.x, g.y, g.w, g.h, 10, TFT_BLACK);
    if (m_tab == Tab::Settings) draw_settings(g);
    else draw_chapters(g);
    draw_tabs(g);
}

void ReaderOverlay::redraw()
{
    if (!m_visible) return;
    M5.Display.startWrite();
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    draw();
    M5.Display.display();
    M5.Display.endWrite();
}
