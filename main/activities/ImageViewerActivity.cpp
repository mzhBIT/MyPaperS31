#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "renderer/TurboJpegDecoder.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"

#include <M5Unified.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

static const char *TAG = "IMAGE_VIEW";
static const int TOP_H = 50;

static std::string short_name(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t max_bytes = 36;
    if (name.size() <= max_bytes) return name;
    return "..." + name.substr(name.size() - max_bytes);
}

static std::string lower_ext(const std::string &path)
{
    size_t p = path.find_last_of('.');
    std::string ext = p == std::string::npos ? std::string() : path.substr(p);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".jpeg") return ".jpg";
    return ext;
}

static bool is_image_ext(const std::string &ext)
{
    return ext == ".jpg" || ext == ".png" || ext == ".bmp";
}

static std::string dir_name(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool probe_png(FILE *fp, int *w, int *h)
{
    uint8_t hdr[24];
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (memcmp(hdr, sig, sizeof(sig)) != 0 || memcmp(hdr + 12, "IHDR", 4) != 0) return false;
    *w = (int)(((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) | ((uint32_t)hdr[18] << 8) | hdr[19]);
    *h = (int)(((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) | ((uint32_t)hdr[22] << 8) | hdr[23]);
    return *w > 0 && *h > 0;
}

static bool probe_bmp(FILE *fp, int *w, int *h)
{
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

static bool probe_jpg(FILE *fp, int *w, int *h, bool *progressive)
{
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

static bool probe_image(const std::string &path, const std::string &ext, int *w, int *h, bool *progressive)
{
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

class ImageViewerActivity : public Activity {
public:
    explicit ImageViewerActivity(const char *path) : _path(path ? path : ""), _ext(lower_ext(_path)) {}

    void on_create() override
    {
        load_siblings();
        redraw();
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK || key == APP_KEY_OK || key == APP_KEY_EXTRA) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP) {
            move_image(-1);
            return true;
        }
        if (key == APP_KEY_DOWN) {
            move_image(1);
            return true;
        }
        return true;
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || event.type == TouchEventType::Tap) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::SwipeLeft) {
            move_image(1);
            return true;
        }
        if (event.type == TouchEventType::SwipeRight) {
            move_image(-1);
            return true;
        }
        return false;
    }

private:
    std::string _path;
    std::string _ext;
    std::vector<std::string> _images;
    int _index = 0;

    void load_siblings()
    {
        _images.clear();
        _index = 0;
        std::string dir = dir_name(_path);
        DIR *dp = opendir(dir.c_str());
        if (dp) {
            struct dirent *ent = nullptr;
            while ((ent = readdir(dp)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string full = dir == "/" ? ("/" + std::string(ent->d_name)) : (dir + "/" + ent->d_name);
                struct stat st = {};
                if (stat(full.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;
                if (is_image_ext(lower_ext(full))) _images.push_back(full);
            }
            closedir(dp);
        }
        std::sort(_images.begin(), _images.end(), [](const std::string &a, const std::string &b) {
            return strcasecmp(a.c_str(), b.c_str()) < 0;
        });
        for (int i = 0; i < (int)_images.size(); ++i) {
            if (_images[i] == _path) {
                _index = i;
                return;
            }
        }
        if (_images.empty()) {
            _images.push_back(_path);
        } else {
            _images.push_back(_path);
            _index = (int)_images.size() - 1;
        }
    }

    void move_image(int delta)
    {
        if (_images.size() <= 1) return;
        int next = _index + delta;
        if (next < 0) next = (int)_images.size() - 1;
        if (next >= (int)_images.size()) next = 0;
        if (next == _index) return;
        _index = next;
        _path = _images[_index];
        _ext = lower_ext(_path);
        redraw();
    }

    void draw_topbar()
    {
        int w = M5.Display.width();
        M5.Display.fillRect(0, 0, w, TOP_H, TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        system_font::draw_left(M5.Display, "<", 8, 3, TFT_BLACK);
        system_font::draw_center(M5.Display, system_font::ellipsize(short_name(_path), w - 170), w / 2, 3, TFT_BLACK);
        if (_images.size() > 1) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%d/%d", _index + 1, (int)_images.size());
            system_font::draw_right(M5.Display, buf, w - 8, 3, TFT_BLACK);
        }
        M5.Display.drawFastHLine(0, TOP_H - 1, w, TFT_BLACK);
    }

    void draw_message(const char *msg)
    {
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        system_font::draw_center(M5.Display, msg, M5.Display.width() / 2, TOP_H + 120);
    }

    bool matches_screen_aspect(int iw, int ih, int sw, int sh) const
    {
        if (iw <= 0 || ih <= 0 || sw <= 0 || sh <= 0) return false;
        int64_t lhs = (int64_t)iw * sh;
        int64_t rhs = (int64_t)ih * sw;
        int64_t diff = lhs > rhs ? lhs - rhs : rhs - lhs;
        int64_t base = std::max<int64_t>(lhs, rhs);
        return diff * 1000 <= base * 5; // within 0.5%
    }

    void redraw()
    {
        int64_t t0 = esp_timer_get_time();
        int w = M5.Display.width();
        int h = M5.Display.height();

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);

        int iw = 0;
        int ih = 0;
        bool progressive = false;
        if (!probe_image(_path, _ext, &iw, &ih, &progressive)) {
            draw_topbar();
            draw_message(ui_locale::text(ui_locale::TextId::UnsupportedImage));
        } else {
            bool exact_screen = iw == w && ih == h;
            bool full_screen = exact_screen || matches_screen_aspect(iw, ih, w, h);
            int max_w = w;
            int max_h = full_screen ? h : h - TOP_H;
            float scale = exact_screen ? 1.0f : std::min((float)max_w / (float)iw, (float)max_h / (float)ih);
            if (scale <= 0.0f) scale = 1.0f;
            int draw_w = std::max(1, (int)(iw * scale));
            int draw_h = std::max(1, (int)(ih * scale));
            int x = std::max(0, (w - draw_w) / 2);
            int y = (full_screen ? 0 : TOP_H) + std::max(0, (max_h - draw_h) / 2);
            if (!full_screen) draw_topbar();

            int64_t td = esp_timer_get_time();
            bool ok = false;
            turbo_jpeg::DrawStats software_stats;
            const char *decoder = "file";
            if (_ext == ".jpg" && progressive) {
                decoder = "turbo-progressive";
                ok = turbo_jpeg::draw_file(M5.Display, _path.c_str(), x, y,
                                           draw_w, draw_h, &software_stats);
            } else if (_ext == ".jpg") {
                ok = M5.Display.drawJpgFile(_path.c_str(), x, y, draw_w, draw_h,
                                             0, 0, scale, scale);
                if (!ok) {
                    decoder = "turbo-fallback";
                    ok = turbo_jpeg::draw_file(M5.Display, _path.c_str(), x, y,
                                               draw_w, draw_h, &software_stats);
                }
            }
            else if (_ext == ".png") ok = M5.Display.drawPngFile(_path.c_str(), x, y, draw_w, draw_h, 0, 0, scale, scale);
            else if (_ext == ".bmp") ok = M5.Display.drawBmpFile(_path.c_str(), x, y, draw_w, draw_h, 0, 0, scale, scale);

            ESP_LOGI(TAG,
                     "draw path=%s ext=%s src=%dx%d draw=%dx%d scale=%.3f fullscreen=%d exact=%d draw=%lld ok=%d progressive=%d decoder=%s sw_decode=%lld sw_push=%lld budget=%u",
                     _path.c_str(),
                     _ext.c_str(),
                     iw,
                     ih,
                     draw_w,
                     draw_h,
                     (double)scale,
                     full_screen ? 1 : 0,
                     exact_screen ? 1 : 0,
                     (esp_timer_get_time() - td) / 1000,
                     ok ? 1 : 0,
                     progressive ? 1 : 0,
                     decoder,
                     software_stats.decode_ms,
                     software_stats.push_ms,
                     (unsigned)software_stats.memory_budget);
            if (!ok) draw_message(ui_locale::text(ui_locale::TextId::ImageDecodeFailed));
        }

        M5.Display.display();
        M5.Display.endWrite();
        ESP_LOGI(TAG, "redraw total=%lld ms", (esp_timer_get_time() - t0) / 1000);
    }
};

void launch_image_viewer(const char *path)
{
    ActivityManager::instance().push(new ImageViewerActivity(path));
}
