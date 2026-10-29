#include "activities/ReaderOverlay.h"
#include "ui/ListFocus.h"

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
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

namespace {

constexpr int PANEL_PAD = 12;
constexpr int STEP_BOX_W = 96;
constexpr int STEP_BOX_GAP = 10;
constexpr int VALUE_W = 100;
constexpr int CHAPTER_INDEX_H = 52;
constexpr int PANEL_RADIUS = 10;
constexpr const char *FONT_PREVIEW_CN = "同一个世界，同一个梦想";
constexpr const char *FONT_PREVIEW_EN = "One world, One dream";

const char *base_name(const char *path)
{
    if (!path || !path[0]) return ui_locale::text(ui_locale::TextId::DefaultFont);
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void setting_text(int index, script_conversion::Mode script_mode, bool script_failed,
                  reader_writing_mode::Mode writing_mode, ReaderProfile profile,
                  std::string *name, std::string *value)
{
    ReaderStyle style = reader_style_get(profile);
    char buf[64] = {};
    switch (index) {
    case 0:
        *name = ui_locale::text(ui_locale::TextId::Font);
        *value = base_name(reader_font_path(profile));
        break;
    case 1:
        *name = ui_locale::text(ui_locale::TextId::FontSize);
        snprintf(buf, sizeof(buf), "%d", style.font_size);
        *value = buf;
        break;
    case 2:
        *name = ui_locale::text(ui_locale::TextId::LetterSpacing);
        snprintf(buf, sizeof(buf), "%d", style.letter_spacing);
        *value = buf;
        break;
    case 3:
        *name = ui_locale::text(ui_locale::TextId::LineSpacing);
        snprintf(buf, sizeof(buf), "%d", style.line_spacing);
        *value = buf;
        break;
    case 4:
        *name = ui_locale::text(ui_locale::TextId::FirstLineIndent);
        snprintf(buf, sizeof(buf), "%d", style.first_line_indent);
        *value = buf;
        break;
    case 5:
        *name = ui_locale::text(ui_locale::TextId::ParagraphGap);
        snprintf(buf, sizeof(buf), "%.1f", style.paragraph_spacing * 0.1f);
        *value = buf;
        break;
    case 6:
        *name = ui_locale::text(ui_locale::TextId::PageMargin);
        snprintf(buf, sizeof(buf), "%d", (style.margin_left + style.margin_right) / 2);
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

void beauty_text(int index, bool weread_mode, ReaderProfile profile, std::string *name, std::string *value)
{
    ReaderStyle style = reader_style_get(profile);
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
        7, // Auto page
        8, // Page animation
    };
    const int ordered_index = weread_mode && index > 5 ? index - 1 : index;
    const int base_index = ordered_index >= 0 && ordered_index < 9
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
        *value = reader_background_enabled(profile)
                   ? base_name(reader_background_path(profile))
                   : ui_locale::text(ui_locale::TextId::Off);
        break;
    case 3:
        *name = ui_locale::text(ui_locale::TextId::BookCover);
        *value = reader_book_cover_enabled(profile)
                   ? base_name(reader_book_cover_path(profile))
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
    case 8:
        *name = ui_locale::text(ui_locale::TextId::PageAnimation);
        *value = ui_locale::text(reader_page_animation_mode(profile)==ReaderAnimationMode::Slow ?
            ui_locale::TextId::AnimationSlow : reader_page_animation_mode(profile)==ReaderAnimationMode::Fast ?
            ui_locale::TextId::AnimationFast : ui_locale::TextId::Off);
        break;
    case 7: {
        *name = ui_locale::text(ui_locale::TextId::AutoPage);
        const int seconds = reader_auto_page_seconds(profile);
        if (seconds == 0) *value = ui_locale::text(ui_locale::TextId::Off);
        else {
            snprintf(buf, sizeof(buf), "%d%s", seconds, ui_locale::text(ui_locale::TextId::SecondsShort));
            *value = buf;
        }
        break;
    }
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
    static constexpr int kBeautyOrder[] = {4, 6, 5, 0, 1, 2, 3, 7, 8};
    if (m_weread_mode && index == 5) return -1;
    const int ordered_index = m_weread_mode && index > 5 ? index - 1 : index;
    return ordered_index >= 0 && ordered_index < 9
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
    if (m_profile==ReaderProfile::Rss) g.y=std::max(g.y,80);
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
    m_fonts_visible = false;
    if (!m_visible) m_pending_progress = -1;
    m_visible = true;
    select_tab(tab);
}

void ReaderOverlay::close()
{
    reset_font_preview();
    m_fonts_visible = false;
    m_fonts.clear();
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
    ReaderStyle style = reader_style_get(m_profile);
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
            style.letter_spacing += delta;
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
            style.margin_left = (style.margin_left + style.margin_right) / 2 + delta * 5;
            style.margin_right = style.margin_left;
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
        case 7:
            reader_auto_page_set_seconds(reader_auto_page_seconds(m_profile) + delta * 5, m_profile);
            return; // Timing-only setting: no text repagination needed.
        case 8:
            reader_page_animation_set_mode(static_cast<ReaderAnimationMode>(
                (static_cast<int>(reader_page_animation_mode(m_profile))+(delta<0 ? 2 : 1))%3),m_profile);
            return;
        default:
            return;
        }
    } else {
        return;
    }
    reader_style_set(style, m_profile);
    reader_style_save(m_profile);
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
                        beauty_index == 4 || beauty_index == 5 || beauty_index == 8));
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
    if (m_tab == Tab::Settings && m_setting_focus == 0) {
        open_fonts();
        return Action::None;
    }
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
    if (m_fonts_visible) {
        if (key == APP_KEY_BACK || key == APP_KEY_EXTRA) return_from_fonts();
        else if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            move_font_focus(key == APP_KEY_UP ? -1 : 1);
            redraw();
        } else if (key == APP_KEY_OK) choose_font();
        return Action::None;
    }
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
    if (m_fonts_visible) {
        if (event.type == TouchEventType::Back) return_from_fonts();
        else if (event.type == TouchEventType::SwipeUp || event.type == TouchEventType::SwipeDown) {
            move_font_focus((event.type == TouchEventType::SwipeUp ? 1 : -1) * font_rows());
            redraw();
        } else if (event.type == TouchEventType::Tap) {
            if (event.y >= g.tab_y && event.y < g.y + g.h &&
                event.x >= g.x + g.w / 2 && event.x < g.x + g.w) {
                choose_font();
            } else if (event.x < g.x || event.x >= g.x + g.w || event.y < g.y || event.y >= g.tab_y) {
                return_from_fonts();
            } else {
                const int top = g.body_y + CHAPTER_INDEX_H;
                const int row_h = std::max(72, system_font::line_height() + 24);
                const int row = (event.y - top) / row_h;
                if (event.y >= top && row < font_rows() && m_font_scroll + row < int(m_fonts.size())) {
                    m_font_focus = m_font_scroll + row;
                    prepare_font_preview();
                    move_font_focus(0);
                    redraw();
                }
            }
        }
        return Action::None;
    }
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
        if (m_tab == Tab::Settings && index == 0) {
            open_fonts();
            return Action::None;
        }
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

int ReaderOverlay::font_rows() const
{
    const auto g = geometry();
    return std::max(1, (g.body_h - CHAPTER_INDEX_H - font_preview_height()) /
        std::max(72, system_font::line_height() + 24));
}

void ReaderOverlay::move_font_focus(int delta)
{
    if (m_fonts.empty()) { m_font_focus = m_font_scroll = 0; return; }
    m_font_focus = std::clamp(m_font_focus + delta, 0, int(m_fonts.size()) - 1);
    prepare_font_preview();
    const int rows = font_rows();
    if (m_font_focus < m_font_scroll) m_font_scroll = m_font_focus;
    if (m_font_focus >= m_font_scroll + rows) m_font_scroll = m_font_focus - rows + 1;
    m_font_scroll = std::clamp(m_font_scroll, 0, std::max(0, int(m_fonts.size()) - rows));
}

void ReaderOverlay::open_fonts()
{
    reset_font_preview();
    m_fonts.clear();
    m_font_save_failed = false;
    m_font_focus = m_font_scroll = 0;
    DIR *dir = opendir("/sdcard/fonts");
    if (dir) {
        while (auto *entry = readdir(dir)) {
            const char *ext = strrchr(entry->d_name, '.');
            if (!ext || (strcasecmp(ext, ".ttf") && strcasecmp(ext, ".otf") && strcasecmp(ext, ".bin"))) continue;
            std::string path = std::string("/sdcard/fonts/") + entry->d_name;
            struct stat st = {};
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) m_fonts.push_back(std::move(path));
        }
        closedir(dir);
    }
    std::sort(m_fonts.begin(), m_fonts.end());
    const std::string current = reader_font_path(m_profile);
    auto found = std::find(m_fonts.begin(), m_fonts.end(), current);
    if (found == m_fonts.end()) found = std::find_if(m_fonts.begin(), m_fonts.end(),
        [&](const std::string &path) { return strcmp(base_name(path.c_str()), base_name(current.c_str())) == 0; });
    if (found != m_fonts.end()) m_font_focus = int(found - m_fonts.begin());
    prepare_font_preview();
    m_font_scroll = std::max(0, m_font_focus - font_rows() / 2);
    move_font_focus(0);
    m_fonts_visible = true;
    redraw();
}

void ReaderOverlay::return_from_fonts()
{
    reset_font_preview();
    m_fonts_visible = false;
    m_fonts.clear();
    m_tab = Tab::Settings;
    m_setting_focus = 0;
    redraw();
}

void ReaderOverlay::choose_font()
{
    if (m_fonts.empty()) return;
    const auto &path = m_fonts[m_font_focus];
    if (path != reader_font_path(m_profile)) {
        if (!reader_font_set(path.c_str(), m_profile)) {
            m_font_save_failed = true;
            redraw();
            return;
        }
        // The owner reloads the text renderer and repaginates when this overlay closes.
        m_settings_dirty = true;
    }
    return_from_fonts();
}

void ReaderOverlay::draw_fonts(const Geometry &g)
{
    prepare_font_preview();
    const int rows = font_rows();
    const int row_h = std::max(72, system_font::line_height() + 24);
    const int top = g.body_y + CHAPTER_INDEX_H;
    const int left = g.x + PANEL_PAD, width = g.w - PANEL_PAD * 2;
    const char *title = ui_locale::text(m_font_save_failed ? ui_locale::TextId::ReaderFontSaveFailed : ui_locale::TextId::Fonts);
    system_font::draw_center(M5.Display, title, g.x + g.w / 2,
        g.body_y + std::max(1, (CHAPTER_INDEX_H - system_font::line_height()) / 2));
    M5.Display.drawFastHLine(left, top, width, TFT_BLACK);
    if (m_fonts.empty()) {
        const auto hint = system_font::ellipsize(ui_locale::text(ui_locale::TextId::NoFontsHint), width - 24);
        system_font::draw_center(M5.Display, hint, g.x + g.w / 2, top + 32);
    }
    for (int row = 0; row < rows && m_font_scroll + row < int(m_fonts.size()); ++row) {
        const int index = m_font_scroll + row, y = top + row * row_h;
        list_focus::draw(M5.Display, left, y, row_h, index == m_font_focus);
        M5.Display.drawFastHLine(left, y + row_h - 1, width, TFT_BLACK);
        const auto name = system_font::ellipsize(base_name(m_fonts[index].c_str()), width - list_focus::text_inset() - 64);
        const int text_y = y + std::max(1, (row_h - system_font::line_height()) / 2);
        system_font::draw_left(M5.Display, name, left + list_focus::text_inset(), text_y);
        if (m_fonts[index] == reader_font_path(m_profile))
            system_font::draw_right(M5.Display, "*", left + width - 12, text_y);
    }
    draw_font_preview(g);
    M5.Display.drawFastHLine(g.x, g.tab_y, g.w, TFT_BLACK);
    M5.Display.drawFastVLine(g.x + g.w / 2, g.tab_y, g.tab_h, TFT_BLACK);
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ReaderFontReturn), g.x + g.w / 4,
        g.tab_y + std::max(1, (g.tab_h - system_font::line_height()) / 2));
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ReaderFontUse), g.x + g.w * 3 / 4,
        g.tab_y + std::max(1, (g.tab_h - system_font::line_height()) / 2));
}

void ReaderOverlay::reset_font_preview()
{
    m_font_preview.end();
    m_preview_path.clear();
    m_preview_size = 0;
    m_preview_ready = false;
}

void ReaderOverlay::prepare_font_preview()
{
    const std::string path = m_fonts.empty() ? reader_font_path(m_profile) : m_fonts[m_font_focus];
    const int size = reader_style_get(m_profile).font_size;
    if (path == m_preview_path && size == m_preview_size) return;
    m_preview_path = path;
    m_preview_size = size;
    ReaderTextStyle style;
    style.font_path = m_preview_path.c_str();
    style.font_size = size;
    style.line_spacing = 0;
    style.letter_spacing = 0;
    style.glyph_cache_limit = 96;
    style.advance_cache_limit = 96;
    style.glyph_pool_bytes = 64 * 1024;
    style.font_stream_windows = 4;
    m_preview_ready = m_font_preview.begin(style);
    if (m_preview_ready) m_preview_ready = m_font_preview.preparePage(
        std::string(FONT_PREVIEW_CN) + "\n" + FONT_PREVIEW_EN);
}

int ReaderOverlay::font_preview_height() const
{
    const int line = std::max(reader_style_get(m_profile).font_size * 3 / 2,
        m_preview_ready ? m_font_preview.lineHeight() : 0);
    const auto g = geometry();
    const int maximum = std::max(0, g.body_h - CHAPTER_INDEX_H -
        std::max(72, system_font::line_height() + 24));
    return std::min(maximum, std::max(180, line * 2 + 48));
}

void ReaderOverlay::draw_font_preview(const Geometry &g)
{
    const int top = g.tab_y - font_preview_height();
    const int left = g.x + PANEL_PAD, width = g.w - PANEL_PAD * 2;
    if (g.tab_y - top <= 16) return;
    M5.Display.drawFastHLine(left, top, width, TFT_BLACK);
    if (!m_preview_ready) {
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ReaderFontPreviewFailed),
            g.x + g.w / 2, top + 32);
        return;
    }
    int32_t clip_x, clip_y, clip_w, clip_h;
    M5.Display.getClipRect(&clip_x, &clip_y, &clip_w, &clip_h);
    M5.Display.setClipRect(left + 8, top + 8, width - 16, g.tab_y - top - 16);
    const int line_h = m_font_preview.lineHeight();
    int y = top + std::max(12, (g.tab_y - top - line_h * 2 - 16) / 2);
    for (const char *sample : {FONT_PREVIEW_CN, FONT_PREVIEW_EN}) {
        const int measured = m_font_preview.measureUtf8(sample);
        const int x = left + std::max(8, (width - measured) / 2);
        m_font_preview.drawUtf8Line(M5.Display, sample, x, y, TFT_BLACK);
        y += line_h + 16;
    }
    M5.Display.setClipRect(clip_x, clip_y, clip_w, clip_h);
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
        list_focus::draw(M5.Display, x, row_y0, row_h, index == m_setting_focus);
        if (index + 1 < count) {
            M5.Display.drawFastHLine(x, row_y1, w, TFT_BLACK);
        }

        std::string name;
        std::string value;
        if (m_tab == Tab::Beauty) beauty_text(index, m_weread_mode, m_profile, &name, &value);
        else setting_text(index, m_script_mode, m_script_load_failed,
                          m_writing_mode, m_profile, &name, &value);
        int text_y = row_y0 + std::max(1, (row_h - system_font::line_height()) / 2);
        const int max_name_w = std::max(40, w - STEP_BOX_W * 2 - STEP_BOX_GAP * 2 - VALUE_W
                                           - list_focus::text_inset() - 20);
        system_font::draw_left(M5.Display, system_font::ellipsize(name, max_name_w),
                               x + list_focus::text_inset(), text_y, fg);

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
                            beauty_index == 4 || beauty_index == 5 || beauty_index == 8));
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
        constexpr uint16_t bg = TFT_WHITE;
        constexpr uint16_t fg = TFT_BLACK;
        M5.Display.fillRect(g.x + PANEL_PAD, row_y0 + 1, g.w - PANEL_PAD * 2, row_h - 1, bg);
        list_focus::draw(M5.Display, g.x + PANEL_PAD, row_y0, row_h, selected);
        if (i + 1 < rows && index + 1 < count) {
            M5.Display.drawFastHLine(g.x + PANEL_PAD, row_y1, g.w - PANEL_PAD * 2, TFT_BLACK);
        }
        int depth = m_chapters.depth ? std::max(0, m_chapters.depth(m_chapters.context, index)) : 0;
        std::string title = m_chapters.title ? m_chapters.title(m_chapters.context, index) : std::string();
        int text_x = g.x + PANEL_PAD + list_focus::text_inset() + std::min(depth, 4) * 18;
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
    const int count = m_chapters_enabled ? 3 : 2;
    const Tab tabs[] = {Tab::Settings, Tab::Beauty, Tab::Chapters};
    const ui_locale::TextId labels[] = {ui_locale::TextId::Settings,
        ui_locale::TextId::Beauty, ui_locale::TextId::Chapters};
    M5.Display.drawFastHLine(g.x, g.tab_y, g.w, TFT_BLACK);
    const int text_y = g.tab_y + std::max(2, (g.tab_h - system_font::line_height()) / 2);
    const int marker_h = std::min(g.tab_h - 8, std::max(48, system_font::line_height() + 16));
    for (int i = 0; i < count; ++i) {
        // Match the existing touch partitions, including their remainder pixels.
        const int left = g.x + i * g.w / count;
        const int right = g.x + (i + 1) * g.w / count;
        if (i) M5.Display.drawFastVLine(left, g.tab_y, g.tab_h, TFT_BLACK);
        list_focus::draw(M5.Display, left + 10,
            g.tab_y + (g.tab_h - marker_h) / 2, marker_h, m_tab == tabs[i]);
        const int text_left = left + 10 + list_focus::text_inset();
        const int text_right = right - 10;
        const auto label = system_font::ellipsize(ui_locale::text(labels[i]),
            std::max(1, text_right - text_left));
        system_font::draw_center(M5.Display, label, (text_left + text_right) / 2,
            text_y, TFT_BLACK);
    }
}

void ReaderOverlay::draw()
{
    if (!m_visible) return;
    Geometry g = geometry();
    M5.Display.fillRoundRect(g.x, g.y, g.w, g.h, PANEL_RADIUS, TFT_WHITE);
    if (m_fonts_visible) draw_fonts(g);
    else if (m_tab == Tab::Settings) draw_settings(g);
    else if (m_tab == Tab::Beauty) draw_beauty(g);
    else draw_chapters(g);
    if (!m_fonts_visible) draw_tabs(g);
    M5.Display.drawRoundRect(g.x, g.y, g.w, g.h, PANEL_RADIUS, TFT_BLACK);
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
