#include "activities/Activity.h"
#include "activities/HomeActivity.h"
#include "ui/BatteryStatus.h"
#include "activities/BookshelfActivity.h"
#include "activities/ActivityManager.h"
#include "activities/DeepSeekActivity.h"
#include "activities/MusicActivity.h"
#include "activities/MapActivity.h"
#include "activities/SmsActivity.h"
#include "activities/PhoneActivity.h"
#include "activities/StocksActivity.h"
#include "activities/CalculatorActivity.h"
#include "activities/EmulatorActivity.h"
#include "activities/CameraActivity.h"
#include "activities/PlaceholderActivity.h"
#include "activities/RadioActivity.h"
#include "activities/SettingActivity.h"
#include "activities/TransferActivity.h"
#include "activities/WifiTransferActivity.h"
#include "activities/WereadActivity.h"
#include "recent_books.h"
#include "renderer/CoverThumbnail.h"
#include "renderer/P4JpegDecoder.h"
#include "renderer/TurboJpegDecoder.h"
#include "ui/home_icons_1bpp.h"
#include "ui/ShelfStatusBar.h"
#include "wifi/wifi_setting.h"
#include "ui/home_preferences.h"
#include "ui/UiLocale.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "time/time_basis.h"
#include <M5Unified.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>
#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_MYPAPER_S31_CAMERA
#include "boards/MyPaperS31/camera/CameraS31.h"
#endif

extern void launch_file_browser(const char *start_path);
extern void launch_rss_reader();
extern void launch_reader(const char *path);
extern void launch_epub_reader(const char *path);
extern void launch_mobi_reader(const char *path);
extern void launch_fb2_reader(const char *path);
extern void launch_markdown_reader(const char *path);

static const char *TAG = "HOME";
static bool g_quality_on_return = false;

void request_home_quality_refresh() { g_quality_on_return = true; }

static const int HEADER_H = shelf_status_bar::height;
static const int COVER_H  = 260;
static const int ROW_H    = 80;
static const int ROW_GAP  = 8;
static const int SIDE_PAD = 20;
static const int MENU_CNT = 12;
static const int HOME_PAGE_CNT = 2;
static const int HOME_DESIGN_W = 684;
static const int HOME_DESIGN_H = 1216;

struct MenuItem { ui_locale::TextId text_id; void (*action)(); };
static void go_browse()  { launch_file_browser("/sdcard"); }
static void go_bookshelf();
static void go_radio()   { launch_radio(); }
static void go_rss()     { launch_rss_reader(); }
static void go_transfer(){ launch_transfer(); }
static void go_settings(){ launch_settings(); }
static void go_clock()   { ActivityManager::instance().push(new PlaceholderActivity(ui_locale::text(ui_locale::TextId::Clock))); }
static void go_music()   { launch_music(); }
static void go_images()  { launch_file_browser("/sdcard"); }
static void go_deepseek(){ launch_deepseek(); }
static void go_weread()  { launch_weread(); }
static void go_map()     { launch_map(); }
#if !CONFIG_IDF_TARGET_ESP32S31
static void go_sms()     { launch_sms(); }
static void go_phone()   { launch_phone(); }
#endif
static void go_stocks()  { launch_stocks(); }
static void go_calculator() { launch_calculator(); }
#if CONFIG_IDF_TARGET_ESP32S31
static void go_emulator() { launch_emulator(); }
#if CONFIG_MYPAPER_S31_CAMERA
static void go_camera() { launch_camera(); }
#endif
#endif

static MenuItem g_menu[HOME_PAGE_CNT][MENU_CNT] = {
    {
        {ui_locale::TextId::Clock, go_clock},
        {ui_locale::TextId::Transfer, go_transfer},
        {ui_locale::TextId::Images, go_images},
        {ui_locale::TextId::Files, go_browse},
        {ui_locale::TextId::Book, go_bookshelf},
        {ui_locale::TextId::News, go_rss},
        {ui_locale::TextId::Music, go_music},
        {ui_locale::TextId::Radio, go_radio},
        {ui_locale::TextId::Settings, go_settings},
        {ui_locale::TextId::Map, go_map},
        {ui_locale::TextId::DeepSeek, go_deepseek},
        {ui_locale::TextId::WeRead, go_weread},
    },
    {
#if !CONFIG_IDF_TARGET_ESP32S31
        {ui_locale::TextId::Messages, go_sms},
        {ui_locale::TextId::Phone, go_phone},
#endif
        {ui_locale::TextId::Stocks, go_stocks}, {ui_locale::TextId::Calculator, go_calculator},
#if CONFIG_IDF_TARGET_ESP32S31
        {ui_locale::TextId::Emulator, go_emulator},
#if CONFIG_MYPAPER_S31_CAMERA
        {ui_locale::TextId::Camera, go_camera},
#else
        {ui_locale::TextId::Count, nullptr},
#endif
#else
        {ui_locale::TextId::Count, nullptr}, {ui_locale::TextId::Count, nullptr},
#endif
        {ui_locale::TextId::Count, nullptr}, {ui_locale::TextId::Count, nullptr},
        {ui_locale::TextId::Count, nullptr}, {ui_locale::TextId::Count, nullptr},
        {ui_locale::TextId::Count, nullptr}, {ui_locale::TextId::Count, nullptr},
#if CONFIG_IDF_TARGET_ESP32S31
        {ui_locale::TextId::Count, nullptr}, {ui_locale::TextId::Count, nullptr},
#endif
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

static HomeIconId g_icons[HOME_PAGE_CNT][MENU_CNT] = {
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
#if !CONFIG_IDF_TARGET_ESP32S31
        HomeIconId::Messages, HomeIconId::Phone,
#endif
        HomeIconId::Stocks, HomeIconId::Calculator,
#if CONFIG_IDF_TARGET_ESP32S31
        HomeIconId::Emulator,
#if CONFIG_MYPAPER_S31_CAMERA
        HomeIconId::Camera,
#else
        HomeIconId::Count,
#endif
#else
        HomeIconId::Count, HomeIconId::Count,
#endif
        HomeIconId::Count, HomeIconId::Count,
        HomeIconId::Count, HomeIconId::Count, HomeIconId::Count, HomeIconId::Count,
#if CONFIG_IDF_TARGET_ESP32S31
        HomeIconId::Count, HomeIconId::Count,
#endif
    },
};

static void configure_optional_apps() {
#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_MYPAPER_S31_CAMERA
    // Keep actions/icons aligned and pack every visible item at the front.
    // Both drawing and hit testing use this same contiguous menu array.
    for (int page = 0; page < HOME_PAGE_CNT; ++page) {
        int visible = 0;
        for (int index = 0; index < MENU_CNT; ++index) {
            if (!g_menu[page][index].action) continue;
            if (g_menu[page][index].text_id == ui_locale::TextId::Camera &&
                !mypaper::board::s31::camera::available()) continue;
            g_menu[page][visible] = g_menu[page][index];
            g_icons[page][visible++] = g_icons[page][index];
        }
        while (visible < MENU_CNT) {
            g_menu[page][visible] = {ui_locale::TextId::Count, nullptr};
            g_icons[page][visible++] = HomeIconId::Count;
        }
    }
#endif
}

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

static bool draw_epub_cover_image(const RecentBook &recent, int x, int y, int w, int h, bool fill = false) {
    if (recent.cover_path.empty() || !file_exists(recent.cover_path)) return false;
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
    float scale = fill ? std::max((float)w / (float)iw, (float)h / (float)ih)
                       : std::min((float)w / (float)iw, (float)h / (float)ih);
    if (scale <= 0.0f) return false;
    int draw_w = std::max(1, (int)((float)iw * scale + 0.5f));
    int draw_h = std::max(1, (int)((float)ih * scale + 0.5f));
    int draw_x = x + (w - draw_w) / 2;
    int draw_y = y + (h - draw_h) / 2;
    struct Clip {
        int32_t x, y, w, h;
        Clip(int32_t cx, int32_t cy, int32_t cw, int32_t ch) {
            M5.Display.getClipRect(&x, &y, &w, &h);
            int right = std::min(x+w, cx+cw), bottom = std::min(y+h, cy+ch);
            cx = std::max(cx,x); cy = std::max(cy,y);
            M5.Display.setClipRect(cx,cy,std::max<int32_t>(0,right-cx),std::max<int32_t>(0,bottom-cy));
        }
        ~Clip() { M5.Display.setClipRect(x,y,w,h); }
    } clip(x,y,w,h);
    if (ext == ".jpg" && !progressive && p4_jpeg::draw_file(M5.Display, image_path.c_str(), draw_x, draw_y, draw_w, draw_h)) {
        return true;
    }

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


bool draw_bookshelf_cover(const RecentBook &book, int x, int y, int w, int h, bool *bin_hit)
{
    if(bin_hit) *bin_hit=false;
    if(book.cover_path.empty()) return false;
    if(cover_thumbnail::draw_bitmap(M5.Display,book.cover_path,x,y,w,h)) {
        if(bin_hit) *bin_hit=true;
        return true;
    }
    if(!draw_epub_cover_image(book, x, y, w, h, true)) return false;
    // Legacy JPEG-only caches migrate lazily, before shelf decorations are drawn.
    cover_thumbnail::save_bitmap(M5.Display,book.cover_path,x,y,w,h);
    return true;
}
static void go_bookshelf() { launch_bookshelf(); }

// ─── Drawing ───────────────────────────────────────────────────────

static void draw_header() {
    M5.Display.fillRect(0, 0, M5.Display.width(), HEADER_H, TFT_WHITE);
    shelf_status_bar::draw();
}

[[maybe_unused]] static void draw_cover() {
    int y = HEADER_H + 20;
    int w = M5.Display.width() - SIDE_PAD * 2;
    RecentBook recent;
    bool has_recent = recent_book_load(&recent);
    std::string title = has_recent ? ellipsize_bytes(recent.title, 38) : ui_locale::text(ui_locale::TextId::NoRecentBook);
    std::string subtitle = has_recent ? ellipsize_bytes(recent_book_base_name(recent.path.c_str()), 52) : ui_locale::text(ui_locale::TextId::BrowseToStartReading);

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
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToContinue), text_x + text_w / 2, y + COVER_H - 54, TFT_WHITE);
    } else {
        system_font::draw_center(M5.Display, system_font::ellipsize(subtitle, w - 40), M5.Display.width() / 2, y + 132, TFT_WHITE);
        system_font::draw_center(M5.Display, has_recent ? ui_locale::text(ui_locale::TextId::TapToContinue) : ui_locale::text(ui_locale::TextId::NoReadingHistory), M5.Display.width() / 2, y + COVER_H - 54, TFT_WHITE);
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
    system_font::draw_center(M5.Display, text, cx, y, fg);
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
    if (!tile.item->action) return;
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
    draw_label_center(ui_locale::text(tile.item->text_id), tile.x + tile.size / 2,
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
        if (!tile.item->action) continue;
        int label_bottom = tile.y + tile.size + sy(20) + system_font::line_height() + 8;
        if (tx >= tile.x - 8 && tx <= tile.x + tile.size + 8 &&
            ty >= tile.y - 8 && ty <= label_bottom) return i;
    }
    return -1;
}

// ─── Activity ──────────────────────────────────────────────────────

class HomeActivity : public Activity {
    bool m_seen_show = false;
    int64_t m_clock_minute = -1;
    int m_clock_offset = 0;
    bool m_clock_24_hour = true;
    uint32_t m_wifi_enabled = 0;
    bool m_battery_available = false;
    bool m_battery_charging = false;

public:
    void on_create() override {
        ESP_LOGI(TAG, "create");
        configure_optional_apps();
        g_page = 0;
        for (int &focus : g_focus) focus = 0;
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_home_page();
        M5.Display.display();
        M5.Display.endWrite();
        remember_clock();
    }

    void on_show() override {
        ESP_LOGI(TAG, "show");
        if (!m_seen_show) {
            m_seen_show = true;
            return;
        }
        M5.Display.startWrite();
        M5.Display.setDither(false);
        const bool quality = g_quality_on_return;
        g_quality_on_return = false;
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
        if (quality) ESP_LOGI(TAG, "one-shot Quality refresh on return to home");
        draw_home_page();
        M5.Display.display();
        M5.Display.endWrite();
        // The queued update captured Quality; only subsequent updates use Text.
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        remember_clock();
    }

    void loop() override {
        if (M5.Display.displayBusy() || touch_input_needs_fast_poll()) return;
        const std::time_t now = std::time(nullptr);
        const int64_t minute = time_basis_system_clock_valid() ? now / 60 : -1;
        const int offset = time_basis_get_offset_minutes();
        const bool hour24 = time_basis_use_24_hour();
        const uint32_t wifi_enabled = shelf_status_bar::network_state();
        const bool battery_available = battery_status::available();
        const bool battery_charging = battery_status::charging();
        if (minute == m_clock_minute && offset == m_clock_offset &&
            hour24 == m_clock_24_hour && wifi_enabled == m_wifi_enabled &&
            battery_available == m_battery_available && battery_charging == m_battery_charging) return;
        if (M5.Display.displayBusy()) return;
        m_clock_minute = minute;
        m_clock_offset = offset;
        m_clock_24_hour = hour24;
        m_wifi_enabled = wifi_enabled;
        m_battery_available = battery_available;
        m_battery_charging = battery_charging;
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        // The panel scans the complete gate array even for a small dirty
        // rectangle. Redraw the full home target to avoid weakening the tiles.
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
            M5.Display.setEpdMode(epd_mode_t::epd_text);
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
            ESP_LOGI(TAG, "page %d menu %d: %s", g_page, idx, ui_locale::text(item.text_id));
            if (item.action) item.action();
            return true;
        }
        return false;
    }

    bool on_navigation_key(int direction) override {
        if (direction < 0 && g_page > 0) switch_page(g_page-1);
        else if (direction > 0 && g_page+1 < HOME_PAGE_CNT) switch_page(g_page+1);
        return true;
    }
    bool on_key(int key) override {
        int &focus = g_focus[g_page];
        if (key == APP_KEY_UP && focus > 0) {
            focus--;
            M5.Display.startWrite();
            M5.Display.setEpdMode(epd_mode_t::epd_text);
            draw_menu();
            draw_page_indicator();
            M5.Display.display();
            M5.Display.endWrite();
            return true;
        }
        if (key == APP_KEY_DOWN && focus + 1 < menu_count(g_page)) {
            focus++;
            M5.Display.startWrite();
            M5.Display.setEpdMode(epd_mode_t::epd_text);
            draw_menu();
            draw_page_indicator();
            M5.Display.display();
            M5.Display.endWrite();
            return true;
        }
        if (key == APP_KEY_OK) {
            const MenuItem &item = g_menu[g_page][focus];
            ESP_LOGI(TAG, "page %d menu %d: %s", g_page, focus, ui_locale::text(item.text_id));
            if (item.action) item.action();
            return true;
        }
        return false;
    }

private:
    void remember_clock() {
        const std::time_t now = std::time(nullptr);
        m_clock_minute = time_basis_system_clock_valid() ? now / 60 : -1;
        m_clock_offset = time_basis_get_offset_minutes();
        m_clock_24_hour = time_basis_use_24_hour();
        m_wifi_enabled = shelf_status_bar::network_state();
        m_battery_available = battery_status::available();
        m_battery_charging = battery_status::charging();
    }
    static int menu_count(int page) {
        int count = 0;
        while (count < MENU_CNT && g_menu[page][count].action) ++count;
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
