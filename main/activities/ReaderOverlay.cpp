#include "activities/ReaderOverlay.h"

#include "reader_font.h"
#include "reader_style.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "ui/reader_background.h"
#include "ui/reader_book_cover.h"
#include "weread/WereadStore.h"

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
    if (!path || !path[0]) return ui_locale::text(ui_locale::TextId::DefaultFont);
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void setting_text(int index, script_conversion::Mode script_mode, bool script_failed,
                  reader_writing_mode::Mode writing_mode,
                  std::string *name, std::string *value)
{
    ReaderStyle style = reader_style_get();
    char buf[64] = {};
    switch (index) {
    case 0:
        *name = ui_locale::text(ui_locale::TextId::Font);
        *value = base_name(reader_font_path());
        break;
    case 1:
        *name = ui_locale::text(ui_locale::TextId::FontSize);
        snprintf(buf, sizeof(buf), "%d", style.font_size);
        *value = buf;
        break;
    case 2:
        *name = ui_locale::text(ui_locale::TextId::LineSpacing);
        snprintf(buf, sizeof(buf), "%d", style.line_spacing);
        *value = buf;
        break;
    case 3:
        *name = ui_locale::text(ui_locale::TextId::FirstLineIndent);
        snprintf(buf, sizeof(buf), "%d", style.first_line_indent);
        *value = buf;
        break;
    case 4:
        *name = ui_locale::text(ui_locale::TextId::ParagraphGap);
        snprintf(buf, sizeof(buf), "%.1f", style.paragraph_spacing * 0.1f);
        *value = buf;
        break;
    case 5:
        *name = ui_locale::text(ui_locale::TextId::LeftMargin);
        snprintf(buf, sizeof(buf), "%d", style.margin_left);
        *value = buf;
        break;
    case 6:
        *name = ui_locale::text(ui_locale::TextId::RightMargin);
        snprintf(buf, sizeof(buf), "%d", style.margin_right);
        *value = buf;
        break;
    case 7:
        *name = ui_locale::text(ui_locale::TextId::Script);
        *value = script_failed ? ui_locale::text(ui_locale::TextId::DictionaryMissing)
                 : ui_locale::text(script_mode == script_conversion::Mode::Simplified
                     ? ui_locale::TextId::Simplified
                     : script_mode == script_conversion::Mode::Traditional
                         ? ui_locale::TextId::Traditional : ui_locale::TextId::Original);
        break;
    case 8:
        *name = ui_locale::text(ui_locale::TextId::WritingDirection);
        *value = ui_locale::text(writing_mode == reader_writing_mode::Mode::VerticalRl
            ? ui_locale::TextId::VerticalRightToLeft
            : writing_mode == reader_writing_mode::Mode::Horizontal
                ? ui_locale::TextId::Horizontal : ui_locale::TextId::Automatic);
        break;
    default:
        name->clear();
        value->clear();
        break;
    }
}

void beauty_text(int index, bool weread_mode, std::string *name, std::string *value)
{
    ReaderStyle style = reader_style_get();
    char buf[64] = {};
    if (weread_mode && index == 5) {
        *name = ui_locale::text(ui_locale::TextId::PublicComments);
        *value = ui_locale::text(weread::store::public_comments_enabled() ? ui_locale::TextId::On : ui_locale::TextId::Off);
        return;
    }
    static constexpr int kBeautyOrder[] = {
        4, // Refresh
        6, // Full refresh
        5, // Show images
        0, // Underline
        1, // Hide header
        2, // Background
        3, // Book cover
    };
    const int ordered_index = weread_mode && index > 5 ? index - 1 : index;
    const int base_index = ordered_index >= 0 && ordered_index < 7
        ? kBeautyOrder[ordered_index]
        : -1;
    switch (base_index) {
    case 0:
        *name = ui_locale::text(ui_locale::TextId::Underline);
        *value = ui_locale::text(style.underline_type == 1 ? ui_locale::TextId::Solid
                  : style.underline_type == 2 ? ui_locale::TextId::Dashed : ui_locale::TextId::None);
        break;
    case 1:
        *name = ui_locale::text(ui_locale::TextId::HideHeader);
        *value = ui_locale::text(style.hide_header ? ui_locale::TextId::On : ui_locale::TextId::Off);
        break;
    case 2:
        *name = ui_locale::text(ui_locale::TextId::Background);
        *value = reader_background_enabled()
                   ? base_name(reader_background_path())
                   : ui_locale::text(ui_locale::TextId::Off);
        break;
    case 3:
        *name = ui_locale::text(ui_locale::TextId::BookCover);
        *value = reader_book_cover_enabled()
                   ? base_name(reader_book_cover_path())
                   : ui_locale::text(ui_locale::TextId::Off);
        break;
    case 4:
        *name = ui_locale::text(ui_locale::TextId::Refresh);
        *value = reader_style_refresh_mode_name(style.refresh_mode);
        break;
    case 5:
        *name = ui_locale::text(ui_locale::TextId::ShowImages);
        *value = ui_locale::text(style.show_images ? ui_locale::TextId::On : ui_locale::TextId::Off);
        break;
    case 6:
        *name = ui_locale::text(ui_locale::TextId::FullRefresh);
        snprintf(buf, sizeof(buf), "%d", style.full_refresh_pages);
        *value = buf;
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

void ReaderOverlay::set_weread_mode(bool enabled)
{
    m_weread_mode = enabled;
    m_setting_focus = std::max(0, std::min(active_setting_count() - 1, m_setting_focus));
}

void ReaderOverlay::set_book_path(const char *path)
{
    m_book_path = path ? path : "";
    m_script_mode = script_conversion::book_mode(m_book_path.c_str());
    m_writing_mode = reader_writing_mode::book_mode(m_book_path.c_str());
    m_script_load_failed = !script_conversion::prepare(m_script_mode);
    if (m_script_load_failed) m_script_mode = script_conversion::Mode::Original;
}

int ReaderOverlay::beauty_base_index(int index) const
{
    static constexpr int kBeautyOrder[] = {4, 6, 5, 0, 1, 2, 3};
    if (m_weread_mode && index == 5) return -1;
    const int ordered_index = m_weread_mode && index > 5 ? index - 1 : index;
    return ordered_index >= 0 && ordered_index < 7
        ? kBeautyOrder[ordered_index]
        : -1;
}

bool ReaderOverlay::is_public_comments_row(int index) const
{
    return m_weread_mode && index == 5;
}

ReaderOverlay::Geometry ReaderOverlay::geometry() const
{
    int screen_w = M5.Display.width();
    int screen_h = M5.Display.height();
    Geometry g;
    g.x = screen_w / 100;
    g.y = screen_h * 10 / 100;
    g.w = screen_w * 98 / 100;
    const int bottom_gap = std::max(8, screen_h * 2 / 100);
    g.h = screen_h - g.y - bottom_gap;
    g.tab_h = std::max(72, screen_h * 12 / 100);
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
    return std::max(1, (g.body_h - CHAPTER_INDEX_H - PROGRESS_AREA_H) / 66);
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
    if (m_tab == Tab::Settings || m_tab == Tab::Beauty) {
        m_setting_focus = std::max(0, std::min(active_setting_count() - 1, m_setting_focus));
    }
    if (m_tab != Tab::Chapters) return;
    if (m_pending_progress >= 0 && chapter_count() > 0) {
        const int count = chapter_count();
        m_chapter_focus = count == 1 ? 0
            : std::clamp((m_pending_progress * (count - 1) + 50) / 100,
                         0, count - 1);
        m_chapter_scroll = std::max(0, m_chapter_focus - visible_chapter_rows() / 2);
    } else if (m_chapters.current) {
        m_chapter_focus = m_chapters.current(m_chapters.context);
        m_chapter_scroll = std::max(0, m_chapter_focus - visible_chapter_rows() / 2);
    }
    clamp_chapter_scroll();
}

void ReaderOverlay::open(Tab tab)
{
    if (!m_visible) m_pending_progress = -1;
    m_visible = true;
    select_tab(tab);
}

void ReaderOverlay::close()
{
    m_visible = false;
    m_pending_progress = -1;
}

bool ReaderOverlay::commit_pending_seek()
{
    if (m_pending_progress < 0 || !m_chapters.seek) return false;
    const int percent = m_pending_progress;
    m_pending_progress = -1;
    return m_chapters.seek(m_chapters.context, percent);
}

void ReaderOverlay::adjust_setting(int delta)
{
    ReaderStyle style = reader_style_get();
    if (m_tab == Tab::Settings) {
        if (m_setting_focus == 0) return;
        if (m_setting_focus == 7) {
            if (m_book_path.empty()) return;
            int next = (static_cast<int>(m_script_mode) + (delta < 0 ? 2 : 1)) % 3;
            auto mode = static_cast<script_conversion::Mode>(next);
            if (script_conversion::set_book_mode(m_book_path.c_str(), mode)) {
                m_script_mode = mode;
                m_script_load_failed = false;
                m_settings_dirty = true;
            } else {
                m_script_load_failed = true;
            }
            return;
        }
        if (m_setting_focus == 8) {
            if (m_book_path.empty()) return;
            const int next = (static_cast<int>(m_writing_mode) + (delta < 0 ? 2 : 1)) % 3;
            const auto mode = static_cast<reader_writing_mode::Mode>(next);
            if (reader_writing_mode::set_book_mode(m_book_path.c_str(), mode)) {
                m_writing_mode = mode;
                m_settings_dirty = true;
            }
            return;
        }
        switch (m_setting_focus) {
        case 1:
            style.font_size += delta * 2;
            break;
        case 2:
            style.line_spacing += delta * 2;
            break;
        case 3:
            style.first_line_indent += delta;
            break;
        case 4:
            style.paragraph_spacing += delta;
            break;
        case 5:
            style.margin_left += delta * 5;
            break;
        case 6:
            style.margin_right += delta * 5;
            break;
        default:
            return;
        }
    } else if (m_tab == Tab::Beauty) {
        if (is_public_comments_row(m_setting_focus)) {
            const bool enabled = !weread::store::public_comments_enabled();
            if (weread::store::set_public_comments_enabled(enabled)) {
                m_settings_dirty = true;
            }
            return;
        }
        switch (beauty_base_index(m_setting_focus)) {
        case 0:
            style.underline_type = reader_style_next_underline_type(style.underline_type, delta);
            break;
        case 1:
            style.hide_header = style.hide_header ? 0 : 1;
            break;
        case 2:
            return;
        case 3:
            return;
        case 4:
            style.refresh_mode = reader_style_next_refresh_mode(style.refresh_mode, delta);
            break;
        case 5:
            style.show_images = style.show_images ? 0 : 1;
            break;
        case 6:
            style.full_refresh_pages += delta * 5;
            break;
        default:
            return;
        }
    } else {
        return;
    }
    reader_style_set(style);
    reader_style_save();
    m_settings_dirty = true;
}

int ReaderOverlay::active_setting_count() const
{
    return m_tab == Tab::Beauty ? BEAUTY_COUNT + (m_weread_mode ? 1 : 0)
        : SETTING_COUNT - (m_book_path.empty() ? 2 : 0);
}

void ReaderOverlay::move_setting_focus(int delta)
{
    m_setting_focus = std::max(0, std::min(active_setting_count() - 1, m_setting_focus + delta));
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
    int count = active_setting_count();
    int index = (y - g.body_y) * count / g.body_h;
    int cell_x = inner_x;
    int cell_w = inner_w;
    if (index < 0 || index >= count || x < cell_x || x >= cell_x + cell_w) return -1;
    const int beauty_index = m_tab == Tab::Beauty ? beauty_base_index(index) : -1;
    bool navigation_row = (m_tab == Tab::Settings && index == 0) ||
                          (m_tab == Tab::Beauty && (beauty_index == 2 || beauty_index == 3));
    bool has_controls = !navigation_row;
    if (has_controls && direction) {
        int plus_x = cell_x + cell_w - STEP_BOX_W;
        int minus_x = plus_x - STEP_BOX_GAP - VALUE_W - STEP_BOX_GAP - STEP_BOX_W;
        bool toggle = (m_tab == Tab::Settings && (index == 7 || index == 8)) ||
                      (m_tab == Tab::Beauty &&
                       (is_public_comments_row(index) || beauty_index <= 1 ||
                        beauty_index == 4 || beauty_index == 5));
        if (toggle && x >= minus_x && x < plus_x + STEP_BOX_W) {
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
    int list_h = std::max(1, g.body_h - CHAPTER_INDEX_H - PROGRESS_AREA_H);
    if (y < list_y || y >= list_y + list_h) return -1;
    int index = m_chapter_scroll + (y - list_y) * visible_chapter_rows() / list_h;
    return index >= 0 && index < chapter_count() ? index : -1;
}

int ReaderOverlay::hit_progress(int x, int y) const
{
    if (!m_chapters.seek) return -1;
    Geometry g = geometry();
    const int area_y = g.tab_y - PROGRESS_AREA_H;
    if (y < area_y || y >= g.tab_y) return -1;
    const int bar_x = g.x + PANEL_PAD + 24;
    const int bar_w = std::max(1, g.w - (PANEL_PAD + 24) * 2);
    return std::clamp((x - bar_x) * 100 / bar_w, 0, 100);
}

ReaderOverlay::Action ReaderOverlay::activate_setting()
{
    if (m_tab == Tab::Settings && m_setting_focus == 0) return Action::OpenFont;
    if (m_tab == Tab::Beauty && beauty_base_index(m_setting_focus) == 2) {
        return Action::OpenBackground;
    }
    if (m_tab == Tab::Beauty && beauty_base_index(m_setting_focus) == 3) {
        return Action::OpenBookCover;
    }
    adjust_setting(1);
    redraw();
    return Action::None;
}

ReaderOverlay::Action ReaderOverlay::activate_chapter()
{
    if (!m_chapters.activate || chapter_count() <= 0) return Action::None;
    if (m_chapters.activate(m_chapters.context, m_chapter_focus)) {
        m_pending_progress = -1;
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
        if (m_tab == Tab::Settings) select_tab(Tab::Beauty);
        else if (m_tab == Tab::Beauty) select_tab(m_chapters_enabled ? Tab::Chapters : Tab::Settings);
        else select_tab(Tab::Settings);
        redraw();
        return Action::None;
    }
    if (m_tab == Tab::Settings || m_tab == Tab::Beauty) {
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
        if (m_tab == Tab::Settings || m_tab == Tab::Beauty) {
            adjust_setting(event.type == TouchEventType::SwipeLeft ? -1 : 1);
        } else {
            select_tab(event.type == TouchEventType::SwipeLeft ? Tab::Settings : Tab::Beauty);
        }
        redraw();
        return Action::None;
    }
    if (event.type == TouchEventType::SwipeUp || event.type == TouchEventType::SwipeDown) {
        int delta = event.type == TouchEventType::SwipeUp ? 1 : -1;
        if (m_tab == Tab::Settings || m_tab == Tab::Beauty) move_setting_focus(delta);
        else move_chapter_focus(delta * visible_chapter_rows());
        redraw();
        return Action::None;
    }
    if (event.type != TouchEventType::Tap) return Action::None;

    if (event.x < g.x || event.x >= g.x + g.w || event.y < g.y || event.y >= g.y + g.h) {
        return Action::Close;
    }
    if (event.y >= g.tab_y) {
        if (!m_chapters_enabled) {
            select_tab(event.x < g.x + g.w / 2 ? Tab::Settings : Tab::Beauty);
        } else {
            int rel_x = event.x - g.x;
            if (rel_x < g.w / 3) select_tab(Tab::Settings);
            else if (rel_x < g.w * 2 / 3) select_tab(Tab::Beauty);
            else select_tab(Tab::Chapters);
        }
        redraw();
        return Action::None;
    }
    if (event.y < g.body_y) return Action::None;

    if (m_tab == Tab::Settings || m_tab == Tab::Beauty) {
        int direction = 0;
        int index = hit_setting(event.x, event.y, &direction);
        if (index < 0) return Action::None;
        m_setting_focus = index;
        if (m_tab == Tab::Settings && index == 0) return Action::OpenFont;
        if (m_tab == Tab::Beauty && beauty_base_index(index) == 2) {
            return Action::OpenBackground;
        }
        if (m_tab == Tab::Beauty && beauty_base_index(index) == 3) {
            return Action::OpenBookCover;
        }
        if (direction != 0) adjust_setting(direction);
        redraw();
        return Action::None;
    }

    const int percent = hit_progress(event.x, event.y);
    if (percent >= 0) {
        m_pending_progress = percent;
        const int count = chapter_count();
        if (count > 0) {
            m_chapter_focus = count == 1 ? 0
                : std::clamp((percent * (count - 1) + 50) / 100, 0, count - 1);
            clamp_chapter_scroll();
        }
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
    int count = active_setting_count();
    for (int index = 0; index < count; ++index) {
        int x = inner_x;
        int w = inner_w;
        int row_y0 = g.body_y + index * g.body_h / count;
        int row_y1 = g.body_y + (index + 1) * g.body_h / count;
        int row_h = row_y1 - row_y0;
        constexpr uint16_t bg = TFT_WHITE;
        constexpr uint16_t fg = TFT_BLACK;
        M5.Display.fillRect(x, row_y0 + 1, w, row_h - 1, bg);
        if (index + 1 < count) {
            M5.Display.drawFastHLine(x, row_y1, w, TFT_BLACK);
        }

        std::string name;
        std::string value;
        if (m_tab == Tab::Beauty) beauty_text(index, m_weread_mode, &name, &value);
        else setting_text(index, m_script_mode, m_script_load_failed,
                          m_writing_mode, &name, &value);
        int text_y = row_y0 + std::max(1, (row_h - system_font::line_height()) / 2);
        const int max_name_w = std::max(40, w - STEP_BOX_W * 2 - STEP_BOX_GAP * 2 - VALUE_W - 32);
        system_font::draw_left(M5.Display, system_font::ellipsize(name, max_name_w), x + 12, text_y, fg);

        const int beauty_index = m_tab == Tab::Beauty ? beauty_base_index(index) : -1;
        if ((m_tab == Tab::Settings && index == 0) ||
            (m_tab == Tab::Beauty && (beauty_index == 2 || beauty_index == 3))) {
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
            bool toggle = (m_tab == Tab::Settings && (index == 7 || index == 8)) ||
                          (m_tab == Tab::Beauty &&
                           (is_public_comments_row(index) || beauty_index <= 1 ||
                            beauty_index == 4 || beauty_index == 5));
            if (toggle) {
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

void ReaderOverlay::draw_beauty(const Geometry &g)
{
    draw_settings(g);
}

void ReaderOverlay::draw_chapters(const Geometry &g)
{
    int count = chapter_count();
    clamp_chapter_scroll();
    int rows = visible_chapter_rows();
    int list_y = g.body_y + CHAPTER_INDEX_H;
    int list_h = std::max(1, g.body_h - CHAPTER_INDEX_H - PROGRESS_AREA_H);
    char position[40] = {};
    if (count > 0) snprintf(position, sizeof(position), "%s  %d / %d",
                            ui_locale::text(ui_locale::TextId::Chapter), m_chapter_focus + 1, count);
    else snprintf(position, sizeof(position), "%s", ui_locale::text(ui_locale::TextId::NoChapterDirectory));
    int index_text_y = g.body_y + std::max(1, (CHAPTER_INDEX_H - system_font::line_height()) / 2);
    system_font::draw_center(M5.Display, position, g.x + g.w / 2, index_text_y, TFT_BLACK);
    M5.Display.drawFastHLine(g.x + PANEL_PAD, list_y, g.w - PANEL_PAD * 2, TFT_BLACK);

    for (int i = 0; count > 0 && i < rows; ++i) {
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

    const int progress_y = g.tab_y - PROGRESS_AREA_H;
    M5.Display.drawFastHLine(g.x + PANEL_PAD, progress_y,
                             g.w - PANEL_PAD * 2, TFT_BLACK);
    const int percent = m_pending_progress >= 0
        ? m_pending_progress
        : std::clamp(m_chapters.progress ? m_chapters.progress(m_chapters.context) : 0,
                     0, 100);
    char progress_label[32] = {};
    snprintf(progress_label, sizeof(progress_label), "%s  %d%%",
             ui_locale::text(ui_locale::TextId::Progress), percent);
    system_font::draw_center(M5.Display, progress_label, g.x + g.w / 2,
                             progress_y + 10, TFT_BLACK);
    const int bar_x = g.x + PANEL_PAD + 24;
    const int bar_w = std::max(1, g.w - (PANEL_PAD + 24) * 2);
    const int bar_y = progress_y + 76;
    M5.Display.drawFastHLine(bar_x, bar_y, bar_w, TFT_DARKGRAY);
    M5.Display.drawFastHLine(bar_x, bar_y + 1, bar_w, TFT_DARKGRAY);
    const int filled = bar_w * percent / 100;
    if (filled > 0) {
        M5.Display.fillRect(bar_x, bar_y - 1, filled, 4, TFT_BLACK);
    }
    const int knob_x = bar_x + std::min(bar_w - 1, filled);
    M5.Display.fillCircle(knob_x, bar_y, 7, TFT_BLACK);
}

void ReaderOverlay::draw_tabs(const Geometry &g)
{
    if (!m_chapters_enabled) {
        int half = g.w / 2;
        M5.Display.drawFastHLine(g.x, g.tab_y, g.w, TFT_BLACK);
        M5.Display.drawFastVLine(g.x + half, g.tab_y, g.tab_h, TFT_BLACK);
        bool settings = m_tab == Tab::Settings;
        M5.Display.fillRect(g.x + 1, g.tab_y + 1, half - 1, g.tab_h - 2,
                            settings ? TFT_BLACK : TFT_WHITE);
        M5.Display.fillRect(g.x + half + 1, g.tab_y + 1, g.w - half - 2, g.tab_h - 2,
                            settings ? TFT_WHITE : TFT_BLACK);
        int text_y = g.tab_y + std::max(2, (g.tab_h - system_font::line_height()) / 2);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Settings), g.x + half / 2, text_y,
                                 settings ? TFT_WHITE : TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Beauty), g.x + half + (g.w - half) / 2, text_y,
                                 settings ? TFT_BLACK : TFT_WHITE);
        return;
    }

    int third = g.w / 3;
    int x0 = g.x;
    int x1 = g.x + third;
    int x2 = g.x + third * 2;
    int x3 = g.x + g.w;
    M5.Display.drawFastHLine(g.x, g.tab_y, g.w, TFT_BLACK);
    M5.Display.drawFastVLine(x1, g.tab_y, g.tab_h, TFT_BLACK);
    M5.Display.drawFastVLine(x2, g.tab_y, g.tab_h, TFT_BLACK);
    bool settings = m_tab == Tab::Settings;
    bool beauty = m_tab == Tab::Beauty;
    bool chapters = m_tab == Tab::Chapters;
    M5.Display.fillRect(x0 + 1, g.tab_y + 1, x1 - x0 - 1, g.tab_h - 2,
                        settings ? TFT_BLACK : TFT_WHITE);
    M5.Display.fillRect(x1 + 1, g.tab_y + 1, x2 - x1 - 1, g.tab_h - 2,
                        beauty ? TFT_BLACK : TFT_WHITE);
    M5.Display.fillRect(x2 + 1, g.tab_y + 1, x3 - x2 - 2, g.tab_h - 2,
                        chapters ? TFT_BLACK : TFT_WHITE);
    int text_y = g.tab_y + std::max(2, (g.tab_h - system_font::line_height()) / 2);
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Settings), x0 + (x1 - x0) / 2, text_y,
                             settings ? TFT_WHITE : TFT_BLACK);
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Beauty), x1 + (x2 - x1) / 2, text_y,
                             beauty ? TFT_WHITE : TFT_BLACK);
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Chapters), x2 + (x3 - x2) / 2, text_y,
                             chapters ? TFT_WHITE : TFT_BLACK);
}

void ReaderOverlay::draw()
{
    if (!m_visible) return;
    Geometry g = geometry();
    M5.Display.fillRoundRect(g.x, g.y, g.w, g.h, 10, TFT_WHITE);
    M5.Display.drawRoundRect(g.x, g.y, g.w, g.h, 10, TFT_BLACK);
    if (m_tab == Tab::Settings) draw_settings(g);
    else if (m_tab == Tab::Beauty) draw_beauty(g);
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
