#include "BasicReader.h"
#include "TxtStream.h"
#include "boards/MyPaperS31/Display.h"
#include "renderer/TextRenderer.h"
#include "storage/storage_sdcard.h"
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if CONFIG_MYPAPER_S31_BASIC_READER && CONFIG_ESP_MAIN_TASK_STACK_SIZE < 32768
#error "S31 FreeType reader requires CONFIG_ESP_MAIN_TASK_STACK_SIZE >= 32768 (16 KiB raster pool plus call stack)"
#endif

namespace mypaper::board::s31 {
namespace {
struct Entry { std::string name; bool directory; };
bool extension(const std::string& name, const char* ext) {
    const size_t len = strlen(ext);
    return name.size() >= len && !strcasecmp(name.c_str() + name.size() - len, ext);
}
class BasicReader {
    TextRenderer text;
    TxtStream book;
    std::vector<Entry> entries;
    std::vector<std::string> fonts{"builtin:system-ui"};
    std::vector<uint32_t> history;
    std::vector<std::string> lines;
    std::string directory = "/sdcard/books", title, error;
    size_t font_index = 0, list_page = 0;
    uint32_t offset = 0, next = 0;
    bool reading = false, mounted = false;
    int row_height = 64, visible_rows = 1;

    void find_fonts() {
        DIR* dir = opendir("/sdcard/fonts");
        if (!dir) return;
        while (auto* item = readdir(dir)) {
            const std::string name = item->d_name;
            if (extension(name, ".ttf") || extension(name, ".otf")) {
                const std::string path = "/sdcard/fonts/" + name;
                struct stat st = {};
                if (!stat(path.c_str(), &st) && S_ISREG(st.st_mode)) fonts.push_back(path);
            }
            if (fonts.size() >= 128) break;
        }
        closedir(dir);
        std::sort(fonts.begin() + 1, fonts.end());
    }
    bool setup_font() {
        ReaderTextStyle style;
        style.font_path = fonts[font_index].c_str();
        style.font_size = 36;
        style.glyph_cache_limit = 512;
        style.glyph_pool_bytes = 384 * 1024;
        style.font_stream_windows = 16;
        if (text.begin(style)) return true;
        ESP_LOGW("S31_READER", "font unavailable: %s; using built-in subset",
                 style.font_path);
        font_index = 0;
        style.font_path = fonts[0].c_str();
        return text.begin(style);
    }
    std::string clipped(std::string value, int width) {
        while (!value.empty() && text.measureUtf8(value) > width) {
            size_t p = value.size() - 1;
            while (p && (static_cast<unsigned char>(value[p]) & 0xc0) == 0x80) --p;
            value.resize(p);
        }
        return value;
    }
    void label(const std::string& value, int x, int y, int width) {
        const std::string visible = clipped(value, width);
        text.preparePage(visible);
        text.drawUtf8Line(display(), visible, x, y, TFT_BLACK);
    }
    void scan() {
        entries.clear(); list_page = 0; error.clear();
        DIR* dir = opendir(directory.c_str());
        if (!dir && directory == "/sdcard/books") {
            directory = "/sdcard"; dir = opendir(directory.c_str());
        }
        if (!dir) { error = "Cannot open directory"; return; }
        while (auto* item = readdir(dir)) {
            const std::string name = item->d_name;
            if (name.empty() || name[0] == '.') continue;
            struct stat st = {};
            const std::string path = directory + "/" + name;
            if (stat(path.c_str(), &st)) continue;
            if (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && extension(name, ".txt")))
                entries.push_back({name, S_ISDIR(st.st_mode)});
            if (entries.size() >= 512) { error = "First 512 entries shown"; break; }
        }
        closedir(dir);
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.directory != b.directory ? a.directory : a.name < b.name;
        });
        ESP_LOGI("S31_READER", "browse path=%s entries=%u", directory.c_str(), unsigned(entries.size()));
    }
    void paginate() {
        const int64_t start = esp_timer_get_time();
        lines.clear();
        if (!book.seek(offset)) { error = "Book read failed"; return; }
        const int max_lines = std::max(1, (int(display().height()) - 192) / text.lineHeight());
        std::string line;
        int width = 0;
        size_t processed = 0;
        while (int(lines.size()) < max_lines) {
            const uint32_t pos = book.tell();
            uint32_t cp;
            if (!book.next(cp)) {
                if (!line.empty()) lines.push_back(line);
                break;
            }
            if ((++processed & 255) == 0) vTaskDelay(1);
            if (cp == '\r' || cp == 0) continue;
            if (cp == '\n') {
                lines.push_back(line); line.clear(); width = 0; continue;
            }
            if (cp == '\t') cp = ' ';
            const int advance = text.measureCodepointForLayout(cp);
            if (width + advance > display().width() - 48 && !line.empty()) {
                lines.push_back(line); line.clear(); width = 0;
                if (int(lines.size()) >= max_lines) { book.seek(pos); break; }
            }
            line += TxtStream::utf8(cp); width += advance;
        }
        next = book.tell();
        std::string glyphs;
        for (const auto& item : lines) glyphs += item;
        text.preparePage(glyphs);
        ESP_LOGI("S31_READER", "TXT page offset=%lu next=%lu size=%lu lines=%u encoding=%s prepare=%lld ms",
                 static_cast<unsigned long>(offset), static_cast<unsigned long>(next),
                 static_cast<unsigned long>(book.size()), unsigned(lines.size()), book.encoding(),
                 (esp_timer_get_time() - start) / 1000);
    }
    void draw(bool quality = false) {
        auto& gfx = display();
        gfx.waitDisplay();
        gfx.setEpdMode(quality ? lgfx::epd_mode_t::epd_quality : lgfx::epd_mode_t::epd_text);
        gfx.startWrite();
        gfx.fillScreen(TFT_WHITE);
        const int h = text.lineHeight();
        label("Back", 16, 12, 110);
        label(reading ? title : "S31 TXT Reader", 140, 12, gfx.width() - 160);
        gfx.drawFastHLine(0, h + 24, gfx.width(), TFT_BLACK);
        if (reading) {
            int y = h + 44;
            for (const auto& line : lines) {
                text.drawUtf8Line(gfx, line, 24, y, TFT_BLACK);
                y += h;
            }
        } else if (!mounted) {
            label("SD unavailable. Tap Retry.", 24, 140, gfx.width() - 48);
            label("Retry", 24, 240, 250);
        } else {
            label(directory, 20, h + 40, gfx.width() - 40);
            label("Font: " + fonts[font_index].substr(fonts[font_index].find_last_of('/') + 1),
                  20, h * 2 + 56, gfx.width() - 40);
            const int top = h * 3 + 80;
            row_height = std::max(64, h + 20);
            visible_rows = std::max(1, (int(gfx.height()) - top - 140) / row_height);
            for (int i = 0; i < visible_rows; ++i) {
                const size_t index = list_page * visible_rows + i;
                if (index >= entries.size()) break;
                label((entries[index].directory ? "+ " : "") + entries[index].name,
                      24, top + i * row_height, gfx.width() - 48);
                gfx.drawFastHLine(20, top + (i + 1) * row_height - 6, gfx.width() - 40, TFT_BLACK);
            }
            if (entries.empty()) label("No TXT files", 24, top, gfx.width() - 48);
            if (!error.empty()) label(error, 24, gfx.height() - 138, gfx.width() - 48);
        }
        gfx.drawFastHLine(0, gfx.height() - 90, gfx.width(), TFT_BLACK);
        label("Previous", 24, gfx.height() - 70, 200);
        label("Next", gfx.width() - 160, gfx.height() - 70, 140);
        gfx.display(); gfx.endWrite();
    }
    void tap(int x, int y) {
        const int h = text.lineHeight();
        if (y < h + 30 && x < 140) {
            if (reading) { book.close(); reading = false; history.clear(); lines.clear(); draw(true); }
            else if (directory != "/sdcard") {
                directory.resize(directory.find_last_of('/')); scan(); draw();
            }
            return;
        }
        if (!mounted) {
            mounted = storage_sdcard_mount() == ESP_OK;
            if (mounted) { find_fonts(); scan(); }
            draw(true); return;
        }
        if (reading) {
            const bool previous = x < display().width() / 2;
            if (previous) {
                if (history.empty()) return;
                offset = history.back(); history.pop_back();
            } else {
                if (next >= book.size() || next <= offset) return;
                history.push_back(offset); offset = next;
            }
            paginate(); draw(); return;
        }
        if (y >= h * 2 + 48 && y < h * 3 + 80) {
            font_index = (font_index + 1) % fonts.size();
            setup_font(); draw(true); return;
        }
        if (y >= display().height() - 90) {
            if (x < display().width() / 2) { if (list_page) --list_page; }
            else if ((list_page + 1) * visible_rows < entries.size()) ++list_page;
            draw(); return;
        }
        const int top = h * 3 + 80;
        if (y < top || y >= top + visible_rows * row_height) return;
        const size_t index = list_page * visible_rows + (y - top) / row_height;
        if (index >= entries.size()) return;
        const std::string path = directory + "/" + entries[index].name;
        if (entries[index].directory) { directory = path; scan(); draw(); return; }
        if (!book.open(path)) { error = "Cannot open TXT"; draw(); return; }
        title = entries[index].name; error.clear(); history.clear();
        offset = book.begin(); reading = true;
        paginate(); draw(true);
    }
public:
    [[noreturn]] void run(bool sd_ready) {
        mounted = sd_ready;
        if (mounted) { find_fonts(); scan(); }
        if (fonts.size() > 1) font_index = 1;
        if (!setup_font()) {
            ESP_LOGE("S31_READER", "No usable font; stopped");
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        draw(true);
        ESP_LOGI("S31_READER", "initial draw complete: main stack minimum free=%u bytes",
                 unsigned(uxTaskGetStackHighWaterMark(nullptr)));
        bool previous = false;
        for (;;) {
            if (!display().displayBusy()) {
                uint16_t x = 0, y = 0;
                const bool pressed = display().getTouch(&x, &y);
                if (pressed && !previous) tap(x, y);
                previous = pressed;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
};
}
[[noreturn]] void run_basic_reader(bool sd_ready) {
    // Large renderer lookup tables live in heap rather than the main task stack.
    auto reader = std::make_unique<BasicReader>();
    reader->run(sd_ready);
}
}
