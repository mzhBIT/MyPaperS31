#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/MusicActivity.h"
#include "activities/PlaceholderActivity.h"
#include "activities/RadioActivity.h"
#include "activities/WifiTransferActivity.h"
#include "recent_books.h"
#include "renderer/CoverThumbnail.h"
#include "renderer/P4JpegDecoder.h"
#include "renderer/TurboJpegDecoder.h"
#include "ui/home_icons_1bpp.h"
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

static const char *TAG = "HOME";

static const int HEADER_H = 80;
static const int COVER_H  = 260;
static const int ROW_H    = 80;
static const int ROW_GAP  = 8;
static const int SIDE_PAD = 20;
static const int MENU_CNT = 9;
static const int HOME_DESIGN_W = 684;
static const int HOME_DESIGN_H = 1216;

struct MenuItem { const char *label; void (*action)(); };
static void go_browse()  { launch_file_browser("/sdcard"); }
static void go_recent();
static void go_radio()   { launch_radio(); }
static void go_rss()     { launch_rss_reader(); }
static void go_wifi()    { launch_wifi_transfer(); }
static void go_settings(){ ActivityManager::instance().push(new PlaceholderActivity("Settings")); }
static void go_clock()   { ActivityManager::instance().push(new PlaceholderActivity("Clock")); }
static void go_music()   { launch_music(); }
static void go_images()  { launch_file_browser("/sdcard"); }

static MenuItem g_menu[MENU_CNT] = {
    {"Clock",    go_clock},
    {"Transfer", go_wifi},
    {"Images",   go_images},
    {"Files",    go_browse},
    {"Book",     go_recent},
    {"News",     go_rss},
    {"Music",    go_music},
    {"Radio",    go_radio},
    {"Setting",  go_settings},
};
static int g_focus = 0;

struct HomeTile {
    const MenuItem *item;
    HomeIconId icon;
    int x;
    int y;
    int size;
};

static const HomeIconId g_icons[MENU_CNT] = {
    HomeIconId::Clock,
    HomeIconId::Transfer,
    HomeIconId::Images,
    HomeIconId::Files,
    HomeIconId::Book,
    HomeIconId::News,
    HomeIconId::Music,
    HomeIconId::Radio,
    HomeIconId::Setting,
};

static const int g_tile_x[3] = {52, 268, 484};
static const int g_tile_y[3] = {158, 533, 908};

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
    if (recent.type != "epub" || recent.cover_path.empty() || !file_exists(recent.cover_path)) return false;
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
static constexpr int RECENT_FOOTER_H = 38;
static constexpr int RECENT_PAGE_SIZE = 4;
static constexpr int RECENT_PAD = 22;
static constexpr int RECENT_GAP = 16;

static void recent_card_rect(int slot, int *x, int *y, int *w, int *h)
{
    int screen_w = M5.Display.width();
    int screen_h = M5.Display.height();
    int columns = screen_w >= 900 ? 4 : 2;
    int rows = (RECENT_PAGE_SIZE + columns - 1) / columns;
    int content_y = RECENT_HEADER_H + 12;
    int content_h = screen_h - content_y - RECENT_FOOTER_H - 6;
    int cell_w = (screen_w - RECENT_PAD * 2 - RECENT_GAP * (columns - 1)) / columns;
    int cell_h = (content_h - RECENT_GAP * (rows - 1)) / rows;
    int col = slot % columns;
    int row = slot / columns;
    if (x) *x = RECENT_PAD + col * (cell_w + RECENT_GAP);
    if (y) *y = content_y + row * (cell_h + RECENT_GAP);
    if (w) *w = cell_w;
    if (h) *h = cell_h;
}

class RecentBooksActivity : public Activity {
public:
    void on_create() override
    {
        ESP_LOGI(TAG, "recent books create");
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
                m_focus = idx;
                open_focused();
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
        if (m_focus < 0 || m_focus >= (int)m_books.size()) return;
        const RecentBook &book = m_books[m_focus];
        ESP_LOGI(TAG, "open recent: %s", book.path.c_str());
        if (book.type == "epub") launch_epub_reader(book.path.c_str());
        else launch_reader(book.path.c_str());
    }

    void draw_header()
    {
        int w = M5.Display.width();
        M5.Display.fillRect(0, 0, w, RECENT_HEADER_H, TFT_BLACK);
        system_font::draw_center(M5.Display, "Recent Reading", w / 2, 12, TFT_WHITE);
        if (!m_books.empty()) {
            char count[24];
            std::snprintf(count, sizeof(count), "%d / %d", current_page() + 1, page_count());
            system_font::draw_right(M5.Display, count, w - 20, 12, TFT_WHITE);
        }
    }

    void draw_placeholder_cover(const RecentBook &book, int x, int y, int w, int h)
    {
        int bw = std::min(w - 34, std::max(72, h * 2 / 3));
        int bh = std::min(h - 28, std::max(100, bw * 4 / 3));
        int bx = x + (w - bw) / 2;
        int by = y + (h - bh) / 2;
        M5.Display.drawRoundRect(bx, by, bw, bh, 5, TFT_BLACK);
        M5.Display.drawLine(bx + 12, by, bx + 12, by + bh, TFT_BLACK);
        std::string type = book.type == "epub" ? "EPUB" : "TXT";
        system_font::draw_center(M5.Display, type, bx + bw / 2 + 6,
                                 by + std::max(8, (bh - system_font::line_height()) / 2), TFT_BLACK);
    }

    void draw_card(const RecentBook &book, int index, int slot)
    {
        int x = 0, y = 0, w = 0, h = 0;
        recent_card_rect(slot, &x, &y, &w, &h);
        bool focused = index == m_focus;
        uint16_t bg = focused ? TFT_BLACK : TFT_WHITE;
        uint16_t fg = focused ? TFT_WHITE : TFT_BLACK;
        M5.Display.fillRoundRect(x, y, w, h, 8, bg);
        M5.Display.drawRoundRect(x, y, w, h, 8, TFT_BLACK);

        int cover_x = x + 12;
        int cover_y = y + 12;
        int cover_w = w - 24;
        int cover_h = std::min(h - 112, cover_w * 7 / 5);
        cover_h = std::max(90, cover_h);
        M5.Display.fillRect(cover_x, cover_y, cover_w, cover_h, TFT_WHITE);
        M5.Display.drawRect(cover_x, cover_y, cover_w, cover_h, TFT_BLACK);
        if (!draw_epub_cover_image(book, cover_x + 2, cover_y + 2, cover_w - 4, cover_h - 4)) {
            draw_placeholder_cover(book, cover_x, cover_y, cover_w, cover_h);
        }

        int title_y = cover_y + cover_h + 10;
        std::string title = system_font::ellipsize(book.title, w - 20);
        system_font::draw_center(M5.Display, title, x + w / 2, title_y, fg);
        std::string detail = book.type == "epub" ? "EPUB" : "TXT";
        system_font::draw_center(M5.Display, detail, x + w / 2,
                                 std::min(y + h - system_font::line_height() - 5,
                                          title_y + system_font::line_height() + 2), fg);
    }

    void redraw()
    {
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();

        if (m_books.empty()) {
            int cy = M5.Display.height() / 2;
            system_font::draw_center(M5.Display, "No recent books", M5.Display.width() / 2,
                                     cy - system_font::line_height(), TFT_BLACK);
            system_font::draw_center(M5.Display, "Open a TXT or EPUB file to start reading",
                                     M5.Display.width() / 2, cy + 12, TFT_BLACK);
        } else {
            int first = current_page() * RECENT_PAGE_SIZE;
            for (int slot = 0; slot < RECENT_PAGE_SIZE; ++slot) {
                int idx = first + slot;
                if (idx >= (int)m_books.size()) break;
                draw_card(m_books[idx], idx, slot);
            }
            if (page_count() > 1) {
                system_font::draw_center(M5.Display, "Swipe to view more", M5.Display.width() / 2,
                                         M5.Display.height() - RECENT_FOOTER_H + 1, TFT_BLACK);
            }
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

static void go_recent() { ActivityManager::instance().push(new RecentBooksActivity()); }

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
    bool drew_cover = has_recent && recent.type == "epub" &&
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
        &g_menu[idx],
        g_icons[idx],
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

static void draw_tile_icon(HomeIconId icon, int x, int y, int size, uint16_t fg, uint16_t bg) {
    const uint8_t *data = home_icon_1bpp_data(icon);
    if (!data) return;
    if (size == (int)HOME_ICON_1BPP_WIDTH) {
        M5.Display.drawBitmap(x, y, data, HOME_ICON_1BPP_WIDTH, HOME_ICON_1BPP_HEIGHT, fg, bg);
        return;
    }

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

static void draw_menu() {
    for (int i = 0; i < MENU_CNT; i++) {
        HomeTile tile = tile_at(i);
        bool focused = i == g_focus;
        uint16_t bg = focused ? TFT_BLACK : TFT_WHITE;
        uint16_t fg = focused ? TFT_WHITE : TFT_BLACK;
        int label_y = tile.y + tile.size + sy(20);
        int label_h = system_font::line_height() + 8;

        M5.Display.fillRect(tile.x - 4, tile.y - 4, tile.size + 8, tile.size + label_h + sy(24), TFT_WHITE);
        draw_tile_icon(tile.icon, tile.x, tile.y, tile.size, fg, bg);
        M5.Display.fillRect(tile.x - 8, label_y - 2, tile.size + 16, label_h, bg);
        draw_label_center(tile.item->label, tile.x + tile.size / 2, label_y, tile.size + 28, fg, bg);
    }
}

static int hit_menu(int tx, int ty) {
    for (int i = 0; i < MENU_CNT; i++) {
        HomeTile tile = tile_at(i);
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
        g_focus = 0;
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();
        draw_menu();
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
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();
        draw_menu();
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool on_touch(const TouchEvent &event) override {
        if (event.type != TouchEventType::Tap) return false;

        int tx = event.x;
        int ty = event.y;
        int idx = hit_menu(tx, ty);
        if (idx >= 0 && idx != g_focus) {
            g_focus = idx;
            M5.Display.startWrite();
            draw_menu();
            M5.Display.display();
            M5.Display.endWrite();
            return true;
        }
        if (idx >= 0) {
            ESP_LOGI(TAG, "menu %d: %s", idx, g_menu[idx].label);
            g_menu[idx].action();
            return true;
        }
        return false;
    }

    bool on_key(int key) override {
        if (key == APP_KEY_UP && g_focus > 0) {
            g_focus--;
            M5.Display.startWrite();
            draw_menu();
            M5.Display.display();
            M5.Display.endWrite();
            return true;
        }
        if (key == APP_KEY_DOWN && g_focus + 1 < MENU_CNT) {
            g_focus++;
            M5.Display.startWrite();
            draw_menu();
            M5.Display.display();
            M5.Display.endWrite();
            return true;
        }
        if (key == APP_KEY_OK) {
            ESP_LOGI(TAG, "menu %d: %s", g_focus, g_menu[g_focus].label);
            g_menu[g_focus].action();
            return true;
        }
        return false;
    }
};

// ─── Entry ────────────────────────────────────────────────────────

void launch_home() {
    ActivityManager::instance().set(new HomeActivity());
}
