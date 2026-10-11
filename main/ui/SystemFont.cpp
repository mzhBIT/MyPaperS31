#include "ui/SystemFont.h"
#include "ui/EmbeddedFont.h"
#include "renderer/TextRenderer.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
constexpr const char *NVS_NS = "system_font";
bool initialized = false;
int size_px = system_font::default_size;
std::string selected_path = embedded_font::path;
uint32_t revision = 0;
SemaphoreHandle_t mutex = nullptr;
struct Lock {
    Lock() {
        if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
        if (mutex) xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    }
    ~Lock() { if (mutex) xSemaphoreGiveRecursive(mutex); }
};
// Independent FreeType caches: no reader/prefetch state is borrowed.
struct Bank {
    uint32_t last_used = 0;
    int px = 0;
    uint32_t revision = UINT32_MAX;
    TextRenderer primary, builtin, fallback;
    bool primary_ok = false, builtin_ok = false, fallback_tried = false, fallback_ok = false;
    void clear() {
        primary.end(); builtin.end(); fallback.end();
        px = 0; revision = UINT32_MAX; last_used = 0;
        primary_ok = builtin_ok = fallback_tried = fallback_ok = false;
    }
};
// The shelf has fixed roles plus scaled buttons/navigation/dialogs. Eight banks
// evicted active role sizes even without speculative fit sizes. Twelve retain
// the normal working set; at most 24 SD faces leave 8 FAT handles for other I/O.
std::array<Bank, 12> banks;
uint32_t bank_tick = 0;
bool font_file(const std::string &path) {
    const char *ext = strrchr(path.c_str(), '.');
    struct stat st = {};
    return path.compare(0, 14, "/sdcard/fonts/") == 0 &&
        path.find("/../") == std::string::npos && ext &&
        (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf")) &&
        !stat(path.c_str(), &st) && S_ISREG(st.st_mode) && st.st_size > 0;
}
std::vector<std::string> scan_fonts() {
    std::vector<std::string> fonts{embedded_font::path};
    DIR *dir = opendir("/sdcard/fonts");
    if (dir) {
        while (auto *entry = readdir(dir)) {
            std::string path = std::string("/sdcard/fonts/") + entry->d_name;
            if (font_file(path)) fonts.push_back(std::move(path));
        }
        closedir(dir);
    }
    std::sort(fonts.begin() + 1, fonts.end());
    return fonts;
}
void initialize() {
    if (initialized) return;
    initialized = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t size = size_px;
    if (nvs_get_i32(h, "size", &size) == ESP_OK && size >= system_font::min_size && size <= system_font::max_size)
        size_px = size;
    char path[256] = {};
    size_t length = sizeof(path);
    if (nvs_get_str(h, "path", path, &length) == ESP_OK && path[0]) selected_path = path;
    nvs_close(h);
}
ReaderTextStyle style(const std::string &path, int px) {
    ReaderTextStyle result;
    result.font_path = path.c_str();
    result.font_size = px;
    result.line_spacing = 0;
    result.glyph_cache_limit = 256;
    result.advance_cache_limit = 512;
    result.glyph_pool_bytes = 128 * 1024;
    result.font_stream_windows = 4; // 64 KiB UI stream cache.
    return result;
}
Bank &bank(int px) {
    initialize();
    px = px > 0 ? std::max(8, std::min(system_font::max_size * 2, px)) : size_px;
    for (auto &b : banks) if (b.px == px && b.revision == revision) { b.last_used = ++bank_tick; return b; }
    Bank *slot = &*std::min_element(banks.begin(), banks.end(),
        [](const Bank &a, const Bank &b) { return a.last_used < b.last_used; });
    for (auto &b : banks) if (b.px == 0 || b.revision != revision) { slot = &b; break; }
    // Release all faces, including a previously used SD fallback. Resetting
    // only primary left an unused fallback's file descriptor pinned.
    slot->clear();
    slot->px = px;
    slot->last_used = ++bank_tick;
    slot->revision = revision;
    slot->fallback_tried = slot->fallback_ok = false;
    const std::string resolved = embedded_font::matches(selected_path.c_str()) || !font_file(selected_path)
        ? embedded_font::path : selected_path;
    slot->primary_ok = slot->primary.begin(style(resolved, px));
    slot->builtin_ok = false;
    if (!slot->primary_ok || resolved != embedded_font::path)
        slot->builtin_ok = slot->builtin.begin(style(embedded_font::path, px));
    return *slot;
}
TextRenderer *face_for(Bank &b, uint32_t cp) {
    if (b.primary_ok && b.primary.hasGlyph(cp)) return &b.primary;
    if (b.builtin_ok && b.builtin.hasGlyph(cp)) return &b.builtin;
    // The UI subset cannot cover arbitrary filenames/messages. Open one
    // complete SD font lazily and use it only for missing codepoints.
    if (!b.fallback_tried) {
        b.fallback_tried = true;
        auto fonts = scan_fonts();
        auto preferred = std::find(fonts.begin(), fonts.end(), "/sdcard/fonts/pingxianzhensong.ttf");
        if (preferred == fonts.end()) preferred = std::find(fonts.begin(), fonts.end(), "/sdcard/fonts/Hei.ttf");
        if (preferred != fonts.end()) std::iter_swap(fonts.begin() + 1, preferred);
        for (size_t i = 1; i < fonts.size(); ++i) {
            if (fonts[i] != selected_path && b.fallback.begin(style(fonts[i], b.px))) {
                b.fallback_ok = true;
                break;
            }
        }
    }
    if (b.fallback_ok && b.fallback.hasGlyph(cp)) return &b.fallback;
    return b.primary_ok ? &b.primary : b.builtin_ok ? &b.builtin : nullptr;
}
size_t next(const std::string &text, size_t pos, uint32_t &cp) {
    const unsigned char lead = text[pos++];
    cp = lead;
    const int count = lead < 128 ? 0 : lead >= 0xC2 && lead <= 0xDF ? 1 :
        lead >= 0xE0 && lead <= 0xEF ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : -1;
    if (count < 0) { cp = '?'; return pos; }
    if (count) cp &= (1u << (6 - count)) - 1;
    for (int i = 0; i < count; ++i) {
        if (pos >= text.size() || (static_cast<unsigned char>(text[pos]) & 0xC0) != 0x80) { cp = '?'; return pos; }
        cp = (cp << 6) | (static_cast<unsigned char>(text[pos++]) & 0x3F);
    }
    return pos;
}
void draw(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, int px, uint16_t color,
          uint16_t background = TFT_WHITE) {
    auto &b = bank(px);
    TextRenderer *base = b.primary_ok ? &b.primary : b.builtin_ok ? &b.builtin : nullptr;
    if (!base) return;
    const int baseline = base->baseline();
    for (size_t pos = 0; pos < text.size();) {
        const size_t start = pos;
        uint32_t cp;
        pos = next(text, pos, cp);
        if (cp == '\r' || cp == '\n') continue;
        auto *face = face_for(b, cp);
        if (!face) continue;
        face->setTextBackground(background);
        face->drawUtf8Line(gfx, text.substr(start, pos - start), x, y + baseline - face->baseline(), color);
        face->setTextBackground(TFT_WHITE);
        x += face->measureCodepoint(cp);
    }
}
esp_err_t save(const std::string &path, int px) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "path", path.c_str());
    if (err == ESP_OK) err = nvs_set_i32(h, "size", px);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        selected_path = path; size_px = px; ++revision;
        for (auto &b : banks) b.clear();
    }
    return err;
}
}

namespace system_font {
bool begin() { Lock lock; auto &b = bank(0); return b.primary_ok || b.builtin_ok; }
int pixel_size() { Lock lock; initialize(); return size_px; }
std::string font_path() { Lock lock; initialize(); return selected_path; }
uint32_t generation() { Lock lock; initialize(); return revision; }
std::vector<std::string> available_fonts() { Lock lock; return scan_fonts(); }
esp_err_t set_size(int px) {
    Lock lock; initialize();
    if (px < min_size || px > max_size) return ESP_ERR_INVALID_ARG;
    return px == size_px ? ESP_OK : save(selected_path, px);
}
esp_err_t set_font(const std::string &path) {
    Lock lock; initialize();
    if (path == selected_path) return ESP_OK;
    if (path != embedded_font::path && !font_file(path)) return ESP_ERR_INVALID_ARG;
    TextRenderer probe;
    if (!probe.begin(style(path, size_px))) return ESP_FAIL;
    return save(path, size_px);
}
int line_height(int px) {
    Lock lock; auto &b = bank(px);
    return b.primary_ok ? b.primary.lineHeight() : b.builtin_ok ? b.builtin.lineHeight() : b.px;
}
int measure(const std::string &text, int px) {
    Lock lock; auto &b = bank(px); int width = 0;
    for (size_t pos = 0; pos < text.size();) {
        uint32_t cp; pos = next(text, pos, cp);
        if (cp == '\r' || cp == '\n') continue;
        auto *face = face_for(b, cp);
        width += face ? face->measureCodepoint(cp) : b.px;
    }
    return width;
}
std::string ellipsize(const std::string &text, int width) {
    Lock lock;
    if (width <= 0) return "";
    if (measure(text) <= width) return text;
    const std::string suffix = "...";
    const int suffix_width = measure(suffix);
    if (suffix_width > width) return "";
    std::string out;
    int used = suffix_width;
    for (size_t pos = 0; pos < text.size();) {
        const size_t start = pos; uint32_t cp; pos = next(text, pos, cp);
        const std::string character = text.substr(start, pos - start);
        const int advance = measure(character);
        if (used + advance > width) break;
        out += character; used += advance;
    }
    return out + suffix;
}
void draw_left(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, uint16_t color) {
    Lock lock; draw(gfx, text, x, y, 0, color);
}
void draw_center(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, uint16_t color) {
    Lock lock; draw(gfx, text, x - measure(text) / 2, y, 0, color);
}
void draw_right(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, uint16_t color) {
    Lock lock; draw(gfx, text, x - measure(text), y, 0, color);
}
void draw_left_sized(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, int px, uint16_t color) {
    Lock lock; draw(gfx, text, x, y, px, color);
}
void draw_on_background(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, int px,
                        uint16_t color, uint16_t background) {
    Lock lock; draw(gfx, text, x, y, px, color, background);
}
int centered_y(const std::string &text, int cy, int px) {
    Lock lock;
    auto &b = bank(px);
    auto *base = b.primary_ok ? &b.primary : b.builtin_ok ? &b.builtin : nullptr;
    if (!base) return cy - px / 2;
    int above = 0, below = 0;
    for (size_t pos = 0; pos < text.size();) {
        uint32_t cp; pos = next(text, pos, cp);
        auto *face = face_for(b, cp);
        int a, d;
        if (face && face->glyphInkExtents(cp, a, d)) {
            above = std::max(above, a); below = std::max(below, d);
        }
    }
    return cy + (above - below) / 2 - base->baseline();
}
void draw_right_sized(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, int px, uint16_t color) {
    Lock lock; draw(gfx, text, x - measure(text, px), y, px, color);
}
}
