#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/DeepSeekActivity.h"
#include "activities/MusicActivity.h"
#include "activities/MapActivity.h"
#include "activities/SmsActivity.h"
#include "activities/PhoneActivity.h"
#include "activities/PlaceholderActivity.h"
#include "activities/RadioActivity.h"
#include "activities/SettingActivity.h"
#include "activities/TransferActivity.h"
#include "activities/WifiTransferActivity.h"
#include "activities/WereadActivity.h"
#include "activities/ZhihuActivity.h"
#include "recent_books.h"
#include "renderer/CoverThumbnail.h"
#include "renderer/P4JpegDecoder.h"
#include "renderer/TurboJpegDecoder.h"
#include "ui/home_icons_1bpp.h"
#include "ui/home_preferences.h"
#include "ui/SystemFont.h"
#include <M5Unified.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

extern void launch_file_browser(const char *start_path);
extern void launch_rss_reader();
extern void launch_reader(const char *path);
extern void launch_epub_reader(const char *path);
extern void launch_mobi_reader(const char *path);
extern void launch_fb2_reader(const char *path);
extern void launch_markdown_reader(const char *path);

static const char *TAG = "HOME";

static const int HEADER_H = 80;
static const int COVER_H  = 260;
static const int ROW_H    = 80;
static const int ROW_GAP  = 8;
static const int SIDE_PAD = 20;
static const int MENU_CNT = 12;
static const int HOME_PAGE_CNT = 2;
static const int HOME_DESIGN_W = 684;
static const int HOME_DESIGN_H = 1216;

struct MenuItem { const char *label; void (*action)(); };
static void go_browse()  { launch_file_browser("/sdcard"); }
static void go_bookshelf();
static void go_radio()   { launch_radio(); }
static void go_rss()     { launch_rss_reader(); }
static void go_transfer(){ launch_transfer(); }
static void go_settings(){ launch_settings(); }
static void go_clock()   { ActivityManager::instance().push(new PlaceholderActivity("Clock")); }
static void go_music()   { launch_music(); }
static void go_images()  { launch_file_browser("/sdcard"); }
static void go_deepseek(){ launch_deepseek(); }
static void go_weread()  { launch_weread(); }
static void go_zhihu()   { launch_zhihu(); }
static void go_map()     { launch_map(); }
static void go_sms()     { launch_sms(); }
static void go_phone()   { launch_phone(); }

static MenuItem g_menu[HOME_PAGE_CNT][MENU_CNT] = {
    {
        {"Clock",    go_clock},
        {"Transfer", go_transfer},
        {"Images",   go_images},
        {"Files",    go_browse},
        {"Book",     go_bookshelf},
        {"News",     go_rss},
        {"Music",    go_music},
        {"Radio",    go_radio},
        {"Setting",  go_settings},
        {"Map",      go_map},
        {"DeepSeek", go_deepseek},
        {"WeRead",   go_weread},
    },
    {
        {"ZhiHu", go_zhihu}, {"Messages", go_sms}, {"Phone", go_phone},
        {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr},
        {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr},
        {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr},
    },
};
static int g_page = 0;
static int g_focus[HOME_PAGE_CNT] = {0, 0};

struct HomeTile {
    const MenuItem *item;
    HomeIconId icon;
    int x;
    int y;
    int size;
};

static const HomeIconId g_icons[HOME_PAGE_CNT][MENU_CNT] = {
    {
        HomeIconId::Clock,
        HomeIconId::Transfer,
        HomeIconId::Images,
        HomeIconId::Files,
        HomeIconId::Book,
        HomeIconId::News,
        HomeIconId::Music,
        HomeIconId::Radio,
        HomeIconId::Setting,
        HomeIconId::Map,
        HomeIconId::DeepSeek,
        HomeIconId::WeChatReading,
    },
    {
        HomeIconId::ZhiHu, HomeIconId::Messages, HomeIconId::Phone,
        HomeIconId::Count, HomeIconId::Count, HomeIconId::Count,
        HomeIconId::Count, HomeIconId::Count, HomeIconId::Count,
        HomeIconId::Count, HomeIconId::Count, HomeIconId::Count,
    },
};

static const int g_tile_x[3] = {52, 268, 484};
static const int g_tile_y[4] = {110, 385, 660, 935};

static std::string ellipsize_bytes(const std::string &s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t pos = 0;
    size_t last_good = 0;
    while (pos < s.size() && pos < max_bytes) {
        unsigned char c = (unsigned char)s[pos];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (pos + len > max_bytes || pos + len > s.size()) break;
        pos += len;
        last_good = pos;
    }
    if (last_good == 0) last_good = std::min(max_bytes, s.size());
    return s.substr(0, last_good) + "...";
}

static std::string lower_ext(const std::string &path) {
    size_t p = path.find_last_of('.');
    std::string ext = p == std::string::npos ? std::string() : path.substr(p);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".jpeg") return ".jpg";
    return ext;
}

static bool file_exists(const std::string &path) {
    struct stat st = {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

static uint16_t be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool probe_png(FILE *fp, int *w, int *h) {
    uint8_t hdr[24];
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (memcmp(hdr, sig, sizeof(sig)) != 0 || memcmp(hdr + 12, "IHDR", 4) != 0) return false;
    *w = (int)(((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) | ((uint32_t)hdr[18] << 8) | hdr[19]);
    *h = (int)(((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) | ((uint32_t)hdr[22] << 8) | hdr[23]);
    return *w > 0 && *h > 0;
}

static bool probe_bmp(FILE *fp, int *w, int *h) {
    uint8_t hdr[26];
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return false;
    if (hdr[0] != 'B' || hdr[1] != 'M') return false;
    int32_t bw = (int32_t)le32(hdr + 18);
    int32_t bh = (int32_t)le32(hdr + 22);
    if (bw == 0 || bh == 0) return false;
    *w = bw < 0 ? -bw : bw;
    *h = bh < 0 ? -bh : bh;
    return *w > 0 && *h > 0;
}

static bool probe_jpg(FILE *fp, int *w, int *h, bool *progressive) {
    if (progressive) *progressive = false;
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    int a = fgetc(fp);
    int b = fgetc(fp);
    if (a != 0xFF || b != 0xD8) return false;

    while (true) {
        int c = fgetc(fp);
        while (c == 0xFF) c = fgetc(fp);
        if (c == EOF || c == 0xD9 || c == 0xDA) return false;
        uint8_t lenbuf[2];
        if (fread(lenbuf, 1, 2, fp) != 2) return false;
        int len = be16(lenbuf);
        if (len < 2) return false;
        bool sof = (c >= 0xC0 && c <= 0xC3) || (c >= 0xC5 && c <= 0xC7) ||
                   (c >= 0xC9 && c <= 0xCB) || (c >= 0xCD && c <= 0xCF);
        if (sof) {
            if (progressive) *progressive = c == 0xC2 || c == 0xC6 || c == 0xCA || c == 0xCE;
            uint8_t dim[5];
            if (fread(dim, 1, sizeof(dim), fp) != sizeof(dim)) return false;
            *h = be16(dim + 1);
            *w = be16(dim + 3);
            return *w > 0 && *h > 0;
        }
        if (fseek(fp, len - 2, SEEK_CUR) != 0) return false;
    }
}

static bool probe_image(const std::string &path, const std::string &ext, int *w, int *h, bool *progressive) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (progressive) *progressive = false;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = false;
    if (ext == ".jpg") ok = probe_jpg(fp, w, h, progressive);
    else if (ext == ".png") ok = probe_png(fp, w, h);
    else if (ext == ".bmp") ok = probe_bmp(fp, w, h);
    fclose(fp);
    return ok;
}

static bool draw_epub_cover_image(const RecentBook &recent, int x, int y, int w, int h) {
    if ((recent.type != "epub" && recent.type != "mobi" && recent.type != "fb2") ||
        recent.cover_path.empty() || !file_exists(recent.cover_path)) return false;
    std::string image_path = recent.cover_path;
    std::string thumbnail_path = cover_thumbnail::cache_path_for(image_path);
    if (image_path != thumbnail_path && cover_thumbnail::ensure(image_path, thumbnail_path)) {
        image_path = thumbnail_path;
    }
    std::string ext = lower_ext(image_path);
    if (ext != ".jpg" && ext != ".png" && ext != ".bmp") return false;

    int iw = 0;
    int ih = 0;
    bool progressive = false;
    if (!probe_image(image_path, ext, &iw, &ih, &progressive)) return false;
    if (ext == ".jpg" && !progressive && p4_jpeg::draw_file(M5.Display, image_path.c_str(), x, y, w, h)) {
        return true;
    }
    float scale = std::min((float)w / (float)iw, (float)h / (float)ih);
    if (scale <= 0.0f) return false;
    int draw_w = std::max(1, (int)((float)iw * scale + 0.5f));
    int draw_h = std::max(1, (int)((float)ih * scale + 0.5f));
    int draw_x = x + std::max(0, (w - draw_w) / 2);
    int draw_y = y + std::max(0, (h - draw_h) / 2);

    bool ok = false;
    if (ext == ".jpg" && progressive) {
        ok = turbo_jpeg::draw_file(M5.Display, image_path.c_str(), draw_x, draw_y, draw_w, draw_h);
    } else if (ext == ".jpg") {
        ok = M5.Display.drawJpgFile(image_path.c_str(), draw_x, draw_y, draw_w, draw_h, 0, 0, scale, scale);
    } else if (ext == ".png") {
        ok = M5.Display.drawPngFile(image_path.c_str(), draw_x, draw_y, draw_w, draw_h, 0, 0, scale, scale);
    } else if (ext == ".bmp") {
        ok = M5.Display.drawBmpFile(image_path.c_str(), draw_x, draw_y, draw_w, draw_h, 0, 0, scale, scale);
    }
    return ok;
}

static constexpr int RECENT_HEADER_H = 76;
static constexpr int RECENT_PAGE_SIZE = 4;
static constexpr int RECENT_PAD = 22;
static constexpr int RECENT_GAP = 10;

static void recent_card_rect(int slot, int *x, int *y, int *w, int *h)
{
    int screen_w = M5.Display.width();
    int screen_h = M5.Display.height();
    int content_y = RECENT_HEADER_H + 12;
    int content_h = screen_h - content_y - 16;
    int row_h = (content_h - RECENT_GAP * (RECENT_PAGE_SIZE - 1)) / RECENT_PAGE_SIZE;
    if (x) *x = RECENT_PAD;
    if (y) *y = content_y + slot * (row_h + RECENT_GAP);
    if (w) *w = screen_w - RECENT_PAD * 2;
    if (h) *h = row_h;
}

class RecentBooksActivity : public Activity {
public:
    void on_create() override
    {
        ESP_LOGI(TAG, "bookshelf create");
        reload();
        redraw();
    }

    void on_show() override
    {
        if (!m_seen_show) {
            m_seen_show = true;
            return;
        }
        reload();
        redraw();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::SwipeUp) {
            change_page(1);
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            change_page(-1);
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y < RECENT_HEADER_H && event.x < 140) {
            ActivityManager::instance().pop();
            return true;
        }

        int first = current_page() * RECENT_PAGE_SIZE;
        for (int slot = 0; slot < RECENT_PAGE_SIZE; ++slot) {
            int idx = first + slot;
            if (idx >= (int)m_books.size()) break;
            int x = 0, y = 0, w = 0, h = 0;
            recent_card_rect(slot, &x, &y, &w, &h);
            if (event.x >= x && event.x < x + w && event.y >= y && event.y < y + h) {
                open_book(idx);
                return true;
            }
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (m_books.empty()) return false;
        if (key == APP_KEY_UP && m_focus > 0) {
            --m_focus;
            redraw();
            return true;
        }
        if (key == APP_KEY_DOWN && m_focus + 1 < (int)m_books.size()) {
            ++m_focus;
            redraw();
            return true;
        }
        if (key == APP_KEY_OK) {
            open_focused();
            return true;
        }
        return false;
    }

private:
    std::vector<RecentBook> m_books;
    std::vector<RecentBookProgress> m_progress;
    std::vector<uint8_t> m_progress_loaded;
    int m_focus = 0;
    bool m_seen_show = false;

    int current_page() const
    {
        return m_focus / RECENT_PAGE_SIZE;
    }

    int page_count() const
    {
        return std::max(1, ((int)m_books.size() + RECENT_PAGE_SIZE - 1) / RECENT_PAGE_SIZE);
    }

    void reload()
    {
        std::string focused_path;
        if (m_focus >= 0 && m_focus < (int)m_books.size()) focused_path = m_books[m_focus].path;
        recent_books_load(&m_books);
        m_progress.assign(m_books.size(), RecentBookProgress{});
        m_progress_loaded.assign(m_books.size(), 0);
        m_focus = 0;
        if (!focused_path.empty()) {
            for (size_t i = 0; i < m_books.size(); ++i) {
                if (m_books[i].path == focused_path) {
                    m_focus = (int)i;
                    break;
                }
            }
        }
    }

    void change_page(int delta)
    {
        if (m_books.empty()) return;
        int page = std::max(0, std::min(page_count() - 1, current_page() + delta));
        int next = page * RECENT_PAGE_SIZE;
        if (next == (current_page() * RECENT_PAGE_SIZE)) return;
        m_focus = std::min(next, (int)m_books.size() - 1);
        redraw();
    }

    void open_focused()
    {
        open_book(m_focus);
    }

    void open_book(int index)
    {
        if (index < 0 || index >= (int)m_books.size()) return;
        const RecentBook &book = m_books[index];
        ESP_LOGI(TAG, "open bookshelf item: %s", book.path.c_str());
        if (book.type == "epub") launch_epub_reader(book.path.c_str());
        else if (book.type == "mobi") launch_mobi_reader(book.path.c_str());
        else if (book.type == "fb2") launch_fb2_reader(book.path.c_str());
        else if (book.type == "markdown") launch_markdown_reader(book.path.c_str());
        else launch_reader(book.path.c_str());
    }

    void draw_header()
    {
        int w = M5.Display.width();
        M5.Display.fillRect(0, 0, w, RECENT_HEADER_H, TFT_WHITE);
        system_font::draw_center(M5.Display, "Bookshelf", w / 2, 12, TFT_BLACK);
        if (!m_books.empty()) {
            char count[24];
            std::snprintf(count, sizeof(count), "%d / %d", current_page() + 1, page_count());
            system_font::draw_right(M5.Display, count, w - 20, 12, TFT_BLACK);
        }
        M5.Display.drawFastHLine(0, RECENT_HEADER_H - 1, w, TFT_BLACK);
    }

    void draw_placeholder_cover(const RecentBook &book, int x, int y, int w, int h)
    {
        M5.Display.fillRoundRect(x, y, w, h, 3, TFT_WHITE);
        M5.Display.drawRoundRect(x, y, w, h, 3, TFT_BLACK);
        int spine_x = x + std::max(7, w / 7);
        M5.Display.drawLine(spine_x, y, spine_x, y + h, TFT_BLACK);
        std::string type = book.type == "epub" ? "EPUB" :
                           (book.type == "mobi" ? "MOBI" :
                            (book.type == "fb2" ? "FB2" :
                             (book.type == "markdown" ? "MD" : "TXT")));
        system_font::draw_center(M5.Display, type, x + w / 2 + 4,
                                 y + std::max(4, (h - system_font::line_height()) / 2), TFT_BLACK);
    }

    const RecentBookProgress &progress_for(int index)
    {
        static const RecentBookProgress empty;
        if (index < 0 || index >= (int)m_books.size()) return empty;
        if (index >= (int)m_progress.size()) {
            m_progress.resize(m_books.size());
            m_progress_loaded.resize(m_books.size(), 0);
        }
        if (!m_progress_loaded[index]) {
            m_progress[index] = recent_book_progress(m_books[index]);
            m_progress[index].percent = std::max(0, std::min(100, m_progress[index].percent));
            m_progress_loaded[index] = 1;
        }
        return m_progress[index];
    }

    void draw_card(const RecentBook &book, int index, int slot)
    {
        int x = 0, y = 0, w = 0, h = 0;
        recent_card_rect(slot, &x, &y, &w, &h);
        constexpr uint16_t fg = TFT_BLACK;
        if (slot > 0) {
            const int divider_y = y - (RECENT_GAP + 1) / 2;
            M5.Display.drawFastHLine(x, divider_y, w, fg);
        }

        int cover_y = y + 11;
        int cover_h = std::max(60, h - 22);
        int cover_w = std::max(40, cover_h * 2 / 3);
        int cover_x = x + 16;
        M5.Display.fillRect(cover_x, cover_y, cover_w, cover_h, TFT_WHITE);
        M5.Display.drawRect(cover_x, cover_y, cover_w, cover_h, TFT_BLACK);
        if (!draw_epub_cover_image(book, cover_x + 2, cover_y + 2, cover_w - 4, cover_h - 4)) {
            draw_placeholder_cover(book, cover_x, cover_y, cover_w, cover_h);
        }

        const RecentBookProgress &progress = progress_for(index);
        bool has_chapters = progress.chapter > 0 && progress.chapter_count > 0;
        int text_x = cover_x + cover_w + 28;
        int text_right = x + w - 32;
        int text_w = std::max(40, text_right - text_x);
        std::string title = system_font::ellipsize(book.title, text_w);
        int line_h = system_font::line_height();
        int line_count = has_chapters ? 3 : 2;
        int gap = std::max(1, (h - line_h * line_count) / (line_count + 1));
        int title_y = y + gap;
        system_font::draw_left(M5.Display, title, text_x, title_y, fg);

        int progress_y = title_y + line_h + gap;
        if (has_chapters) {
            char chapter_text[40];
            std::snprintf(chapter_text, sizeof(chapter_text), "Chapter %d / %d",
                          progress.chapter, progress.chapter_count);
            system_font::draw_left(M5.Display, chapter_text, text_x, progress_y, fg);
            progress_y += line_h + gap;
        }

        char progress_text[32];
        std::snprintf(progress_text, sizeof(progress_text), "Progress %d%%", progress.percent);
        system_font::draw_left(M5.Display, progress_text, text_x, progress_y, fg);

        int bar_x = text_x + system_font::measure(progress_text) + 24;
        int bar_w = std::max(0, text_right - bar_x);
        if (bar_w > 0) {
            int bar_y = progress_y + line_h / 2;
            M5.Display.fillRect(bar_x, bar_y + 1, bar_w, 2, fg);
            int filled = (int)(((int64_t)bar_w * progress.percent + 50) / 100);
            if (filled > 0) M5.Display.fillRect(bar_x, bar_y, filled, 4, fg);
        }
    }

    void redraw()
    {
        M5.Display.startWrite();
        M5.Display.setDither(false);
        // Keep the recent-books view on the text waveform to reduce the
        // noticeable flashing caused by full quality refreshes.
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();

        if (m_books.empty()) {
            int cy = M5.Display.height() / 2;
            system_font::draw_center(M5.Display, "Bookshelf is empty", M5.Display.width() / 2,
                                     cy - system_font::line_height(), TFT_BLACK);
            system_font::draw_center(M5.Display, "Open a TXT, EPUB, MOBI or Markdown file from Files",
                                     M5.Display.width() / 2, cy + 12, TFT_BLACK);
        } else {
            int first = current_page() * RECENT_PAGE_SIZE;
            for (int slot = 0; slot < RECENT_PAGE_SIZE; ++slot) {
                int idx = first + slot;
                if (idx >= (int)m_books.size()) break;
                draw_card(m_books[idx], idx, slot);
            }
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

static void go_bookshelf() { ActivityManager::instance().push(new RecentBooksActivity()); }

// ─── Drawing ───────────────────────────────────────────────────────

static void draw_header() {
    int w = M5.Display.width();

    M5.Display.fillRect(0, 0, w, HEADER_H, TFT_WHITE);
    M5.Display.drawLine(0, HEADER_H - 2, w, HEADER_H - 2, TFT_BLACK);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    system_font::draw_center(M5.Display, "E-Book Reader", w / 2, 18);

    // Time from RTC
    auto dt = M5.Rtc.getDateTime();
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", dt.time.hours, dt.time.minutes);
    system_font::draw_right(M5.Display, tbuf, w - 10, 18);
}

[[maybe_unused]] static void draw_cover() {
    int y = HEADER_H + 20;
    int w = M5.Display.width() - SIDE_PAD * 2;
    RecentBook recent;
    bool has_recent = recent_book_load(&recent);
    std::string title = has_recent ? ellipsize_bytes(recent.title, 38) : "No Recent Book";
    std::string subtitle = has_recent ? ellipsize_bytes(recent_book_base_name(recent.path.c_str()), 52) : "Browse files to start reading";

    // Cover card background (dark)
    M5.Display.fillRoundRect(SIDE_PAD, y, w, COVER_H, 10, TFT_BLACK);
    bool drew_cover = has_recent && (recent.type == "epub" || recent.type == "mobi") &&
                      draw_epub_cover_image(recent, SIDE_PAD + 18, y + 16, 154, COVER_H - 32);

    // Title + author in white text on black
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    if (drew_cover) {
        int text_x = SIDE_PAD + 190;
        int text_w = w - 210;
        system_font::draw_center(M5.Display, system_font::ellipsize(title, text_w), text_x + text_w / 2, y + 74, TFT_WHITE);
    } else {
        system_font::draw_center(M5.Display, system_font::ellipsize(title, w - 40), M5.Display.width() / 2, y + 78, TFT_WHITE);
    }
    if (drew_cover) {
        int text_x = SIDE_PAD + 190;
        int text_w = w - 210;
        system_font::draw_center(M5.Display, system_font::ellipsize(subtitle, text_w), text_x + text_w / 2, y + 132, TFT_WHITE);
        system_font::draw_center(M5.Display, "Tap to continue >", text_x + text_w / 2, y + COVER_H - 54, TFT_WHITE);
    } else {
        system_font::draw_center(M5.Display, system_font::ellipsize(subtitle, w - 40), M5.Display.width() / 2, y + 132, TFT_WHITE);
        system_font::draw_center(M5.Display, has_recent ? "Tap to continue >" : "No reading history", M5.Display.width() / 2, y + COVER_H - 54, TFT_WHITE);
    }
}

static int sx(int v) {
    return (int)((int64_t)v * M5.Display.width() / HOME_DESIGN_W);
}

static int sy(int v) {
    return (int)((int64_t)v * M5.Display.height() / HOME_DESIGN_H);
}

static HomeTile tile_at(int idx) {
    int col = idx % 3;
    int row = idx / 3;
    return HomeTile{
        &g_menu[g_page][idx],
        g_icons[g_page][idx],
        sx(g_tile_x[col]),
        sy(g_tile_y[row]),
        std::min(sx(150), sy(150)),
    };
}

static void draw_label_center(const char *label, int cx, int y, int max_w, uint16_t fg, uint16_t bg) {
    std::string text = system_font::ellipsize(label, max_w);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(fg, bg);
    M5.Display.drawString(text.c_str(), cx - system_font::measure(text) / 2, y,
                          &fonts::lv_font_SourceHanSansNormalCN_36);
}

static bool bitmap_bit(const uint8_t *data, int x, int y) {
    int offset = y * (int)HOME_ICON_1BPP_STRIDE + (x >> 3);
    return (data[offset] & (0x80 >> (x & 7))) != 0;
}

static void draw_tile_icon(HomeIconId icon, int x, int y, int size,
                           uint16_t fg, uint16_t bg, bool show_frame) {
    const uint8_t *data = home_icon_1bpp_data(icon);
    if (!data) return;
    if (size == (int)HOME_ICON_1BPP_WIDTH) {
        M5.Display.drawBitmap(x, y, data, HOME_ICON_1BPP_WIDTH, HOME_ICON_1BPP_HEIGHT, fg, bg);
    } else {
        for (int dy = 0; dy < size; ++dy) {
            int src_y = dy * (int)HOME_ICON_1BPP_HEIGHT / size;
            int run_x = 0;
            bool run_black = bitmap_bit(data, 0, src_y);
            for (int dx = 1; dx <= size; ++dx) {
                int src_x = dx < size ? dx * (int)HOME_ICON_1BPP_WIDTH / size : (int)HOME_ICON_1BPP_WIDTH;
                bool black = dx < size ? bitmap_bit(data, src_x, src_y) : !run_black;
                if (dx == size || black != run_black) {
                    M5.Display.fillRect(x + run_x, y + dy, dx - run_x, 1, run_black ? fg : bg);
                    run_x = dx;
                    run_black = black;
                }
            }
        }
    }

    if (show_frame) {
        // The bitmap contains artwork only; draw the same frame for every icon.
        const int frame_width = std::max(1, size * 3 / (int)HOME_ICON_1BPP_WIDTH);
        for (int inset = 0; inset < frame_width; ++inset) {
            M5.Display.drawRect(x + inset, y + inset,
                                size - inset * 2, size - inset * 2, fg);
        }
    }
}

static void draw_menu_tile(int index, bool show_frame) {
    if (index < 0 || index >= MENU_CNT) return;
    HomeTile tile = tile_at(index);
    if (!tile.item->label) return;
    bool focused = index == g_focus[g_page];
    uint16_t bg = focused ? TFT_BLACK : TFT_WHITE;
    uint16_t fg = focused ? TFT_WHITE : TFT_BLACK;
    int label_y = tile.y + tile.size + sy(20);
    int label_h = system_font::line_height() + 8;

    M5.Display.fillRect(tile.x - 8, tile.y - 4,
                        tile.size + 16, tile.size + label_h + sy(24), TFT_WHITE);
    draw_tile_icon(tile.icon, tile.x, tile.y, tile.size, fg, bg, show_frame);
    M5.Display.fillRect(tile.x - 8, label_y - 2,
                        tile.size + 16, label_h, bg);
    draw_label_center(tile.item->label, tile.x + tile.size / 2,
                      label_y, tile.size + 28, fg, bg);
}

static void draw_menu() {
    const bool show_frame = home_icon_frames_enabled();
    for (int i = 0; i < MENU_CNT; i++) {
        draw_menu_tile(i, show_frame);
    }
}

static void draw_page_indicator() {
    constexpr int radius = 5;
    constexpr int gap = 24;
    int center_x = M5.Display.width() / 2;
    int y = M5.Display.height() - 22;
    int first_x = center_x - gap / 2;
    M5.Display.fillRect(first_x - radius - 3, y - radius - 3,
                        gap + radius * 2 + 6, radius * 2 + 6, TFT_WHITE);
    for (int page = 0; page < HOME_PAGE_CNT; ++page) {
        int x = first_x + page * gap;
        if (page == g_page) M5.Display.fillCircle(x, y, radius, TFT_BLACK);
        else M5.Display.drawCircle(x, y, radius, TFT_BLACK);
    }
}

static void draw_home_page() {
    M5.Display.fillScreen(TFT_WHITE);
    draw_header();
    draw_menu();
    draw_page_indicator();
}

static int hit_menu(int tx, int ty) {
    for (int i = 0; i < MENU_CNT; i++) {
        HomeTile tile = tile_at(i);
        if (!tile.item->label) continue;
        int label_bottom = tile.y + tile.size + sy(20) + system_font::line_height() + 8;
        if (tx >= tile.x - 8 && tx <= tile.x + tile.size + 8 &&
            ty >= tile.y - 8 && ty <= label_bottom) return i;
    }
    return -1;
}

// ─── Activity ──────────────────────────────────────────────────────

class HomeActivity : public Activity {
    bool m_seen_show = false;

public:
    void on_create() override {
        ESP_LOGI(TAG, "create");
        g_page = 0;
        for (int &focus : g_focus) focus = 0;
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_home_page();
        M5.Display.display();
        M5.Display.endWrite();
    }

    void on_show() override {
        ESP_LOGI(TAG, "show");
        if (!m_seen_show) {
            m_seen_show = true;
            return;
        }
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_home_page();
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool on_touch(const TouchEvent &event) override {
        if (event.type == TouchEventType::SwipeLeft && g_page + 1 < HOME_PAGE_CNT) {
            switch_page(g_page + 1);
            return true;
        }
        if (event.type == TouchEventType::SwipeRight && g_page > 0) {
            switch_page(g_page - 1);
            return true;
        }
        // A right swipe starting at the left bezel is promoted to the global
        // Back gesture by touch_input.  On the root home activity it should
        // still return from page 2 to page 1.
        if (event.type == TouchEventType::Back && g_page > 0 && event.dx > 0) {
            switch_page(g_page - 1);
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;

        int tx = event.x;
        int ty = event.y;
        int idx = hit_menu(tx, ty);
        if (idx >= 0 && idx != g_focus[g_page]) {
            g_focus[g_page] = idx;
            M5.Display.startWrite();
            // The current panel wiring scans the complete gate array even for
            // a small dirty rectangle.  Refresh the complete home grid so
            // repeated focus changes do not visually weaken neighbouring
            // tiles while only two of them are redrawn.
            draw_menu();
            M5.Display.display();
            M5.Display.endWrite();
            return true;
        }
        if (idx >= 0) {
            const MenuItem &item = g_menu[g_page][idx];
            ESP_LOGI(TAG, "page %d menu %d: %s", g_page, idx, item.label);
            if (item.action) item.action();
            return true;
        }
        return false;
    }

    bool on_key(int key) override {
        int &focus = g_focus[g_page];
        if (key == APP_KEY_UP && focus > 0) {
            focus--;
            M5.Display.startWrite();
            draw_menu();
            draw_page_indicator();
            M5.Display.display();
            M5.Display.endWrite();
            return true;
        }
        if (key == APP_KEY_DOWN && focus + 1 < menu_count(g_page)) {
            focus++;
            M5.Display.startWrite();
            draw_menu();
            draw_page_indicator();
            M5.Display.display();
            M5.Display.endWrite();
            return true;
        }
        if (key == APP_KEY_OK) {
            const MenuItem &item = g_menu[g_page][focus];
            ESP_LOGI(TAG, "page %d menu %d: %s", g_page, focus, item.label);
            if (item.action) item.action();
            return true;
        }
        return false;
    }

private:
    static int menu_count(int page) {
        int count = 0;
        while (count < MENU_CNT && g_menu[page][count].label) ++count;
        return count;
    }

    static void switch_page(int page) {
        if (page < 0 || page >= HOME_PAGE_CNT || page == g_page) return;
        g_page = page;
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_home_page();
        M5.Display.display();
        M5.Display.endWrite();
        ESP_LOGI(TAG, "page switched: %d/%d", g_page + 1, HOME_PAGE_CNT);
    }
};

// ─── Entry ────────────────────────────────────────────────────────

void launch_home() {
    ActivityManager::instance().set(new HomeActivity());
}
