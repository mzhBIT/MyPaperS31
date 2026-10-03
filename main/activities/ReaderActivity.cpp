#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/ReaderBackgroundPickerActivity.h"
#include "activities/ReaderOverlay.h"
#include "ui/ReaderAutoPage.h"
#include "parser/txt_parser.h"
#include "recent_books.h"
#include "reader_style.h"
#include "renderer/TextRenderer.h"
#include "ui/SystemFont.h"
#include "ui/reader_background.h"
#include "utils/CooperativeYield.h"
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <string>
#include <vector>

static const char *TAG = "READER";
static const int TOP_H = 50;
static TxtBook g_book;
static TxtPage g_page;
static uint32_t g_offset = 0;
static std::vector<uint32_t> g_history;

extern void launch_reader_font_picker();

static epd_mode_t reader_epd_mode() {
    switch (reader_style_get().refresh_mode) {
    case 1:
        return epd_mode_t::epd_quality;
    case 2:
        return epd_mode_t::epd_text;
    case 3:
        return epd_mode_t::epd_fast;
    case 4:
        return epd_mode_t::epd_fastest;
    case 5:
        return epd_mode_t::epd_hd;
    default:
        return epd_mode_t::epd_fast;
    }
}

// ─── Drawing ───────────────────────────────────────────────────────

static bool reader_header_visible()
{
    return reader_style_get().hide_header == 0;
}

static int reader_header_height()
{
    return reader_header_visible() ? TOP_H : 0;
}

static void draw_page_number(int w) {
    if (!reader_header_visible()) return;
    // Clear old page number area then draw new one
    M5.Display.fillRect(w - 170, 0, 170, TOP_H, TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    uint32_t size = g_book.file_size();
    double pct = size ? ((double)g_page.start_offset * 100.0 / (double)size) : 0.0;
    if (pct > 100.0) pct = 100.0;
    char pg[32]; snprintf(pg, sizeof(pg), "%.2f%%", pct);
    system_font::draw_right(M5.Display, pg, w - 5, 3);
}

static void draw_topbar(int w) {
    if (!reader_header_visible()) return;
    M5.Display.fillRect(0, 0, w, TOP_H, TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    system_font::draw_center(M5.Display, system_font::ellipsize(g_book.title, w - 180), w / 2, 3);
    draw_page_number(w);
}

static void draw_text_underline(lgfx::LGFXBase &gfx, int x, int y, int width, int line_h)
{
    ReaderStyle style = reader_style_get();
    if (style.underline_type <= 0 || width <= 0) return;
    int underline_y = y + std::max(1, line_h - 5);
    if (style.underline_type == 1) {
        gfx.drawFastHLine(x, underline_y, width, TFT_BLACK);
    } else {
        constexpr int dash = 12;
        constexpr int gap = 8;
        for (int dx = 0; dx < width; dx += dash + gap) {
            gfx.drawFastHLine(x + dx, underline_y, std::min(dash, width - dx), TFT_BLACK);
        }
    }
}

static void draw_content(int w, int h) {
    const auto &text = g_page.text;
    auto &renderer = reader_text_renderer();
    ReaderStyle style = reader_style_get();

    int x = style.margin_left;
    int y = reader_header_height() + style.margin_top;
    int max_y = h - style.margin_bottom;
    int line_h = renderer.lineHeight();
    if (line_h <= 0) line_h = 48;
    const int justify_width = std::max(100, w - style.margin_left - style.margin_right);

    int64_t t0 = esp_timer_get_time();
    bool prepared = renderer.preparePage(text);
    if (!prepared) {
        ESP_LOGW(TAG, "TTF preparePage reported missing glyphs or render errors");
    }
    int64_t t_prepare = esp_timer_get_time();
    if (g_page.vertical) {
        int right = w - style.margin_right;
        const std::vector<inline_style::Span> no_styles;
        int columns = 0;
        for (const auto &page_line : g_page.lines) {
            if (right <= style.margin_left) break;
            const size_t start = page_line.text_offset;
            const size_t length = page_line.text_length;
            if (start > text.size() || length > text.size() - start) break;
            const std::string column = text.substr(start, length);
            if (!column.empty()) {
                const int column_x = right - line_h;
                const int column_y = y + page_line.x_offset;
                renderer.drawStyledUtf8VerticalColumn(M5.Display, column, no_styles,
                                                       column_x, column_y, line_h, TFT_BLACK);
                if (style.underline_type > 0) {
                    const int length_px = renderer.measureUtf8Vertical(column);
                    if (style.underline_type == 1) {
                        M5.Display.drawFastVLine(column_x - 2, column_y, length_px, TFT_BLACK);
                    } else {
                        for (int dy = 0; dy < length_px; dy += 20) {
                            M5.Display.drawFastVLine(column_x - 2, column_y + dy,
                                std::min(12, length_px - dy), TFT_BLACK);
                        }
                    }
                }
            }
            right -= page_line.height > 0 ? page_line.height : line_h;
            ++columns;
        }
        ESP_LOGI(TAG, "vertical draw: columns=%d prepare=%lld draw=%lld ms",
                 columns, (t_prepare - t0) / 1000,
                 (esp_timer_get_time() - t_prepare) / 1000);
        return;
    }
    int line_count = 0;
    int char_count = 0;
    CooperativeYield cooperative_yield;
    for (const auto &page_line : g_page.lines) {
        if (y >= max_y) break;
        ++line_count;
        cooperative_yield.ifDue();

        const size_t start = page_line.text_offset;
        const size_t length = page_line.text_length;
        if (start > text.size() || length > text.size() - start) {
            ESP_LOGW(TAG, "invalid TXT line range offset=%u length=%u text=%u",
                     (unsigned)page_line.text_offset,
                     (unsigned)page_line.text_length,
                     (unsigned)text.size());
            break;
        }
        std::string line = text.substr(start, length);

        if (!line.empty()) {
            char_count += line.size();
            int text_x = x + page_line.x_offset;
            int underline_w = page_line.justify
                            ? std::max(1, justify_width - (int)page_line.x_offset)
                            : renderer.measureUtf8(line);
            renderer.drawUtf8Line(M5.Display, line, x + page_line.x_offset, y, TFT_BLACK,
                                  page_line.justify
                                      ? std::max(1, justify_width - (int)page_line.x_offset)
                                      : 0);
            draw_text_underline(M5.Display, text_x, y, underline_w,
                                page_line.height > 0 ? page_line.height : line_h);
        }
        y += page_line.height > 0 ? page_line.height : line_h;
    }
    int64_t t1 = esp_timer_get_time();
    auto stats = renderer.stats();
    ESP_LOGI(TAG, "draw_content: prepare=%lld draw=%lld total=%lld ms, %d lines, %d chars, glyphs=%u pool=%u/%u missing=%u",
             (t_prepare - t0) / 1000,
             (t1 - t_prepare) / 1000,
             (t1 - t0) / 1000,
             line_count,
             char_count,
             (unsigned)stats.glyph_count,
             (unsigned)stats.glyph_pool_used,
             (unsigned)stats.glyph_pool_capacity,
             (unsigned)stats.missing_glyphs);
}

// ─── Activity ──────────────────────────────────────────────────────

class ReaderActivity : public Activity {
    bool m_seen_show = false;
    BookReadingTimer m_reading_timer;
    ReaderOverlay m_overlay;
    ReaderAutoPage m_auto_page;
    int m_non_quality_refresh_count = 0;
public:
    ReaderActivity(const char *path) {
        ReaderOverlayChapters chapters;
        chapters.context = this;
        chapters.progress = overlay_progress;
        chapters.seek = overlay_seek_progress;
        m_overlay.set_chapters(chapters);
        m_overlay.set_book_path(path);
        if (g_book.open(path)) {
            recent_book_save(path, "txt", g_book.title.c_str());
            m_reading_timer.configure(path);
        }
        g_history.clear();
        g_offset = g_book.load_progress(&g_history);
        g_page = g_book.read_page(g_offset);
    }
    ~ReaderActivity() {
        m_reading_timer.finish();
        g_book.save_progress(g_page.start_offset, g_history);
    }

    void on_hide() override { m_reading_timer.pause(); m_auto_page.reset(); }

    void on_show() override {
        m_auto_page.reset();
        m_reading_timer.resume();
        if (!m_seen_show) {
            m_seen_show = true;
            return;
        }

        int w = M5.Display.width(), h = M5.Display.height();
        g_book.refresh_layout_settings();
        g_page = g_book.read_page(g_page.start_offset);
        m_overlay.clear_settings_dirty();
        redraw_all(w, h, epd_mode_t::epd_quality);
    }

    void on_create() override {
        int w = M5.Display.width(), h = M5.Display.height();
        epd_mode_t mode = select_page_refresh_mode();
        epd_mode_t default_mode = reader_epd_mode();
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
        reader_background_draw(M5.Display, w, h);

        if (reader_header_visible()) draw_topbar(w);
        draw_content(w, h);
        M5.Display.display();
        M5.Display.endWrite();
        if (mode != default_mode) {
            M5.Display.setEpdMode(default_mode);
        }
    }

    void loop() override {
        const bool allowed = !m_overlay.visible() && !M5.Display.displayBusy() && !g_page.eof;
        if (m_auto_page.due(esp_timer_get_time(), reader_auto_page_seconds(), allowed)) {
            next_page(M5.Display.width(), M5.Display.height());
        }
    }

    bool on_touch(const TouchEvent &event) override {
        m_auto_page.reset();
        int w = M5.Display.width(), h = M5.Display.height();

        if (m_overlay.visible()) {
            handle_overlay_action(m_overlay.on_touch(event), w, h);
            return true;
        }

        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::SwipeLeft) {
            next_page(w, h);
            return true;
        }
        if (event.type == TouchEventType::SwipeRight) {
            prev_page(w, h);
            return true;
        }
        if (event.type == TouchEventType::LongPress) {
            m_overlay.open(ReaderOverlay::Tab::Chapters);
            m_overlay.redraw();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;

        if (event.y < TOP_H + 10 && event.x < 120) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.x >= w / 3 && event.x <= (w * 2) / 3) {
            m_overlay.open(ReaderOverlay::Tab::Settings);
            m_overlay.redraw();
        } else if (event.x > w / 2) {
            next_page(w, h);
        } else {
            prev_page(w, h);
        }
        return true;
    }

    bool on_key(int key) override {
        m_auto_page.reset();
        int w = M5.Display.width(), h = M5.Display.height();

        if (m_overlay.visible()) {
            handle_overlay_action(m_overlay.on_key(key), w, h);
            return true;
        }

        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_EXTRA) {
            m_overlay.open(ReaderOverlay::Tab::Settings);
            m_overlay.redraw();
            return true;
        }
        if (key == APP_KEY_UP) {
            prev_page(w, h);
            return true;
        }
        if (key == APP_KEY_DOWN || key == APP_KEY_OK) {
            next_page(w, h);
            return true;
        }
        return key == APP_KEY_UP || key == APP_KEY_DOWN || key == APP_KEY_OK;
    }

    void next_page(int w, int h) {
        if (g_page.eof || g_page.next_offset == g_page.start_offset) return;
        g_history.push_back(g_page.start_offset);
        if (g_history.size() > 64) {
            g_history.erase(g_history.begin(), g_history.begin() + (g_history.size() - 64));
        }
        g_offset = g_page.next_offset;
        g_page = g_book.read_page(g_offset);
        redraw(w, h);
    }

    void prev_page(int w, int h) {
        if (g_page.start_offset == 0) return;
        if (!g_history.empty()) {
            g_offset = g_history.back();
            g_history.pop_back();
        } else {
            uint32_t prev = g_book.previous_page_offset(g_page.start_offset);
            if (prev == g_page.start_offset) return;
            g_offset = prev;
        }
        g_page = g_book.read_page(g_offset);
        redraw(w, h);
    }

    void redraw(int w, int h) {
        int64_t t0 = esp_timer_get_time();
        epd_mode_t mode = select_page_refresh_mode();
        epd_mode_t default_mode = reader_epd_mode();
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
        if (reader_background_enabled()) {
            reader_background_draw(M5.Display, w, h);
            if (reader_header_visible()) draw_topbar(w);
        } else if (mode == epd_mode_t::epd_quality
                || mode == epd_mode_t::epd_hd) {
            M5.Display.fillScreen(TFT_WHITE);
            if (reader_header_visible()) draw_topbar(w);
        } else {
            // Only clear content area, topbar stays unchanged
            int clear_y = reader_header_height();
            M5.Display.fillRect(0, clear_y, w, h - clear_y, TFT_WHITE);
        }
        int64_t t1 = esp_timer_get_time();
        if (mode != epd_mode_t::epd_quality && mode != epd_mode_t::epd_hd) {
            draw_page_number(w);
        }
        int64_t t2 = esp_timer_get_time();
        draw_content(w, h);
        if (m_overlay.visible()) m_overlay.draw();
        int64_t t3 = esp_timer_get_time();
        M5.Display.display();
        int64_t t4 = esp_timer_get_time();
        M5.Display.endWrite();
        if (mode != default_mode) {
            M5.Display.setEpdMode(default_mode);
        }
        ESP_LOGI(TAG, "redraw: mode=%d clear=%lld pagenum=%lld content=%lld display=%lld total=%lld ms",
                 (int)mode, (t1-t0)/1000, (t2-t1)/1000, (t3-t2)/1000, (t4-t3)/1000, (t4-t0)/1000);
    }

    void redraw_all(int w, int h, epd_mode_t mode = reader_epd_mode()) {
        int64_t t0 = esp_timer_get_time();
        note_page_refresh_mode(mode);
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
        reader_background_draw(M5.Display, w, h);
        if (reader_header_visible()) draw_topbar(w);
        draw_content(w, h);
        if (m_overlay.visible()) m_overlay.draw();
        M5.Display.display();
        M5.Display.endWrite();
        epd_mode_t default_mode = reader_epd_mode();
        if (mode != default_mode) {
            M5.Display.setEpdMode(default_mode);
        }
        ESP_LOGI(TAG, "redraw_all: mode=%d total=%lld ms", (int)mode, (esp_timer_get_time() - t0) / 1000);
    }

private:
    static int overlay_progress(void *)
    {
        const uint32_t size = g_book.file_size();
        if (size == 0) return 0;
        return std::clamp(static_cast<int>(
            (static_cast<uint64_t>(g_page.start_offset) * 100 + size / 2) / size),
            0, 100);
    }

    static bool overlay_seek_progress(void *, int percent)
    {
        percent = std::clamp(percent, 0, 100);
        g_history.clear();
        g_offset = g_book.seek_percent(static_cast<uint8_t>(percent));
        g_page = g_book.read_page(g_offset);
        return true;
    }

    epd_mode_t select_page_refresh_mode()
    {
        ReaderStyle style = reader_style_get();
        epd_mode_t preferred = reader_epd_mode();
        if (preferred == epd_mode_t::epd_quality
         || preferred == epd_mode_t::epd_hd) {
            m_non_quality_refresh_count = 0;
            return preferred;
        }

        int limit = std::min(60, std::max(1, style.full_refresh_pages));
        if (m_non_quality_refresh_count + 1 >= limit) {
            ESP_LOGI(TAG,
                     "force quality refresh: interval=%d preferred=%s offset=%lu",
                     limit,
                     reader_style_refresh_mode_name(style.refresh_mode),
                     (unsigned long)g_page.start_offset);
            m_non_quality_refresh_count = 0;
            return epd_mode_t::epd_quality;
        }

        ++m_non_quality_refresh_count;
        return preferred;
    }

    void note_page_refresh_mode(epd_mode_t mode)
    {
        if (mode == epd_mode_t::epd_quality || mode == epd_mode_t::epd_hd) {
            m_non_quality_refresh_count = 0;
        }
    }

    void handle_overlay_action(ReaderOverlay::Action action, int w, int h)
    {
        if (action == ReaderOverlay::Action::None) return;
        if (action == ReaderOverlay::Action::OpenFont) {
            launch_reader_font_picker();
            return;
        }
        if (action == ReaderOverlay::Action::OpenBackground) {
            launch_reader_background_picker();
            return;
        }
        if (action == ReaderOverlay::Action::OpenBookCover) {
            launch_reader_book_cover_picker();
            return;
        }

        if (action == ReaderOverlay::Action::Close) m_overlay.commit_pending_seek();
        bool apply_layout = m_overlay.settings_dirty();
        m_overlay.close();
        if (apply_layout) {
            g_book.set_script_mode(m_overlay.script_mode());
            g_book.set_writing_mode(m_overlay.writing_mode());
            g_book.refresh_layout_settings();
            g_page = g_book.read_page(g_page.start_offset);
            m_overlay.clear_settings_dirty();
        }
        redraw_all(w, h, epd_mode_t::epd_quality);
    }
};

void launch_reader(const char *path) {
    ActivityManager::instance().push(new ReaderActivity(path));
}
