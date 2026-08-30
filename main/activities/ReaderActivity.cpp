#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/ReaderOverlay.h"
#include "parser/txt_parser.h"
#include "recent_books.h"
#include "reader_style.h"
#include "renderer/TextRenderer.h"
#include "ui/SystemFont.h"
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
    default:
        return epd_mode_t::epd_fast;
    }
}

// ─── Drawing ───────────────────────────────────────────────────────

static void draw_page_number(int w) {
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
    M5.Display.fillRect(0, 0, w, TOP_H, TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    system_font::draw_center(M5.Display, system_font::ellipsize(g_book.title, w - 180), w / 2, 3);
    draw_page_number(w);
}

static void draw_content(int w, int h) {
    const auto &text = g_page.text;
    auto &renderer = reader_text_renderer();
    ReaderStyle style = reader_style_get();

    int x = style.margin_left;
    int y = TOP_H + style.margin_top;
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
    int line_count = 0;
    int char_count = 0;
    for (const auto &page_line : g_page.lines) {
        if (y >= max_y) break;
        if ((line_count++ & 7) == 0) vTaskDelay(1);

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
            renderer.drawUtf8Line(M5.Display, line, x + page_line.x_offset, y, TFT_BLACK,
                                  page_line.justify
                                      ? std::max(1, justify_width - (int)page_line.x_offset)
                                      : 0);
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
    ReaderOverlay m_overlay;
public:
    ReaderActivity(const char *path) {
        if (g_book.open(path)) recent_book_save(path, "txt", g_book.title.c_str());
        g_history.clear();
        g_offset = g_book.load_progress(&g_history);
        g_page = g_book.read_page(g_offset);
    }
    ~ReaderActivity() { g_book.save_progress(g_page.start_offset, g_history); }

    void on_show() override {
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
        M5.Display.startWrite();
        M5.Display.setEpdMode(reader_epd_mode());
        M5.Display.fillScreen(TFT_WHITE);

        draw_topbar(w);
        draw_content(w, h);
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool on_touch(const TouchEvent &event) override {
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
        M5.Display.startWrite();
        M5.Display.setEpdMode(reader_epd_mode());
        // Only clear content area, topbar stays unchanged
        M5.Display.fillRect(0, TOP_H, w, h - TOP_H, TFT_WHITE);
        int64_t t1 = esp_timer_get_time();
        draw_page_number(w);
        int64_t t2 = esp_timer_get_time();
        draw_content(w, h);
        if (m_overlay.visible()) m_overlay.draw();
        int64_t t3 = esp_timer_get_time();
        M5.Display.display();
        int64_t t4 = esp_timer_get_time();
        M5.Display.endWrite();
        ESP_LOGI(TAG, "redraw: clear=%lld pagenum=%lld content=%lld display=%lld total=%lld ms",
                 (t1-t0)/1000, (t2-t1)/1000, (t3-t2)/1000, (t4-t3)/1000, (t4-t0)/1000);
        vTaskDelay(1);
    }

    void redraw_all(int w, int h, epd_mode_t mode = reader_epd_mode()) {
        int64_t t0 = esp_timer_get_time();
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar(w);
        draw_content(w, h);
        if (m_overlay.visible()) m_overlay.draw();
        M5.Display.display();
        M5.Display.endWrite();
        epd_mode_t default_mode = reader_epd_mode();
        if (mode != default_mode) {
            M5.Display.setEpdMode(default_mode);
        }
        ESP_LOGI(TAG, "redraw_all: total=%lld ms", (esp_timer_get_time() - t0) / 1000);
        vTaskDelay(1);
    }

private:
    void handle_overlay_action(ReaderOverlay::Action action, int w, int h)
    {
        if (action == ReaderOverlay::Action::None) return;
        if (action == ReaderOverlay::Action::OpenFont) {
            launch_reader_font_picker();
            return;
        }

        bool apply_layout = m_overlay.settings_dirty();
        m_overlay.close();
        if (apply_layout) {
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
