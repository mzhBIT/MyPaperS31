#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/ReaderOverlay.h"
#include "parser/epub_html.h"
#include "parser/epub_parser.h"
#include "recent_books.h"
#include "renderer/CoverThumbnail.h"
#include "reader_font.h"
#include "reader_style.h"
#include "renderer/DocumentLayout.h"
#include "renderer/P4JpegDecoder.h"
#include "renderer/TextRenderer.h"
#include "renderer/TurboJpegDecoder.h"
#include "storage/app_paths.h"
#include "ui/SystemFont.h"

#include <M5Unified.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include <string>
#include <unordered_map>
#include <vector>

static const char *TAG = "EPUB_READER";
static const int TOP_H = 50;
static const uint32_t EPUB_PROGRESS_MAGIC = 0x45504231; // EPB1
static const uint32_t EPUB_PROGRESS_VERSION = 2;
static const uint32_t EPUB_LAYOUT_VERSION = 5;
static EpubBook g_epub;

extern void launch_reader_font_picker();

class EpubReaderActivity;

static epd_mode_t epub_reader_epd_mode()
{
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

static bool refresh_text_renderer()
{
    ReaderTextStyle text_style;
    ReaderStyle style = reader_style_get();
    text_style.font_path = reader_font_path();
    text_style.font_size = style.font_size;
    text_style.line_spacing = style.line_spacing;
    return reader_text_renderer().begin(text_style);
}

static std::string short_title(const std::string &title)
{
    const size_t max_bytes = 36;
    if (title.size() <= max_bytes) return title;

    size_t pos = 0;
    size_t last_good = 0;
    while (pos < title.size() && pos < max_bytes) {
        unsigned char c = (unsigned char)title[pos];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (pos + len > max_bytes || pos + len > title.size()) break;
        pos += len;
        last_good = pos;
    }
    if (last_good == 0) last_good = max_bytes;
    return title.substr(0, last_good) + "...";
}

static uint32_t hash32_bytes(const void *data, size_t len, uint32_t h = 2166136261u)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash32_string(const std::string &s, uint32_t h = 2166136261u)
{
    return hash32_bytes(s.data(), s.size(), h);
}

static std::string progress_cache_key(const char *path)
{
    size_t h = 0;
    for (const char *p = path; p && *p; ++p) h = h * 31 + (unsigned char)*p;
    char buf[32];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
    return buf;
}

static void ensure_epub_progress_root()
{
    app_paths::ensure_dir(app_paths::epub_progress_dir());
}

static bool file_exists(const std::string &path)
{
    struct stat st = {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

static std::string lower_ext(const std::string &path)
{
    size_t p = path.find_last_of('.');
    std::string ext = p == std::string::npos ? std::string() : path.substr(p);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".jpeg") return ".jpg";
    return ext;
}

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool probe_png_size(FILE *fp, int *w, int *h)
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

static bool probe_bmp_size(FILE *fp, int *w, int *h)
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

static bool probe_jpeg_size(FILE *fp, int *w, int *h, bool *progressive)
{
    if (progressive) *progressive = false;
    if (fseek(fp, 0, SEEK_SET) != 0) return false;
    int a = fgetc(fp);
    int b = fgetc(fp);
    if (a != 0xFF || b != 0xD8) return false;

    while (true) {
        int c = fgetc(fp);
        while (c == 0xFF) c = fgetc(fp);
        if (c == EOF) return false;
        if (c == 0xD9 || c == 0xDA) return false;

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

static bool probe_image_size(const std::string &path, const std::string &ext, int *w, int *h, bool *jpg_progressive = nullptr)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (jpg_progressive) *jpg_progressive = false;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = false;
    if (ext == ".png") ok = probe_png_size(fp, w, h);
    else if (ext == ".jpg") ok = probe_jpeg_size(fp, w, h, jpg_progressive);
    else if (ext == ".bmp") ok = probe_bmp_size(fp, w, h);
    fclose(fp);
    return ok;
}

class EpubReaderActivity : public Activity {
public:
    explicit EpubReaderActivity(const char *path)
    {
        ReaderOverlayChapters chapters;
        chapters.context = this;
        chapters.count = overlay_chapter_count;
        chapters.current = overlay_current_chapter;
        chapters.title = overlay_chapter_title;
        chapters.depth = overlay_chapter_depth;
        chapters.activate = overlay_activate_chapter;
        _overlay.set_chapters(chapters);

        _ok = g_epub.open(path);
        if (_ok) {
            std::string key = progress_cache_key(path);
            ensure_epub_progress_root();
            _progress_path = std::string(app_paths::epub_progress_dir()) + "/" + key + ".pos";
            _image_cache_dir = std::string(app_paths::epub_cache_dir()) + "/" + key;
            app_paths::ensure_dir(app_paths::epub_cache_dir());
            mkdir(_image_cache_dir.c_str(), 0);
            std::string cover_path = ensure_cover_cache();
            recent_book_save(path, "epub", g_epub.title.c_str(), cover_path.c_str());
            refresh_text_renderer();
            _layout_sig = layout_signature();
            int saved_spine = 0;
            uint64_t saved_position = 0;
            if (load_progress(&saved_spine, &saved_position) && load_spine(saved_spine)) {
                _page_index = page_for_position(saved_position);
            } else {
                load_spine(0);
            }
        }
    }

    ~EpubReaderActivity()
    {
        save_progress();
        release_runtime_cache();
        g_epub.close();
    }

    void on_create() override
    {
        redraw_all(epd_mode_t::epd_quality);
    }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        if (_ok) {
            refresh_text_renderer();
            repaginate_current_spine();
        }
        _overlay.clear_settings_dirty();
        redraw_all(epd_mode_t::epd_quality);
    }

    bool on_key(int key) override
    {
        int w = M5.Display.width();
        int h = M5.Display.height();

        if (_overlay.visible()) {
            handle_overlay_action(_overlay.on_key(key));
            return true;
        }

        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_EXTRA) {
            _overlay.open(ReaderOverlay::Tab::Chapters);
            _overlay.redraw();
            return true;
        }
        if (key == APP_KEY_UP) {
            prev_page();
            save_progress();
            redraw(w, h);
            return true;
        }
        if (key == APP_KEY_DOWN || key == APP_KEY_OK) {
            next_page();
            save_progress();
            redraw(w, h);
            return true;
        }
        return true;
    }

    int current_spine_index() const
    {
        return _spine_index;
    }

    bool jump_to_spine(int index)
    {
        if (!_ok || index < 0 || index >= g_epub.spine_count()) return false;
        if (!load_spine(index)) return false;
        _page_index = 0;
        save_progress();
        return true;
    }

    bool on_touch(const TouchEvent &event) override
    {
        int w = M5.Display.width();
        int h = M5.Display.height();

        if (_overlay.visible()) {
            handle_overlay_action(_overlay.on_touch(event));
            return true;
        }

        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::LongPress) {
            _overlay.open(ReaderOverlay::Tab::Chapters);
            _overlay.redraw();
            return true;
        }
        if (!_ok) return false;
        if (event.type == TouchEventType::SwipeLeft) {
            next_page();
            save_progress();
            redraw(w, h);
            return true;
        }
        if (event.type == TouchEventType::SwipeRight) {
            prev_page();
            save_progress();
            redraw(w, h);
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;

        if (event.y < TOP_H + 10 && event.x < 120) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.x >= w / 3 && event.x <= (w * 2) / 3) {
            _overlay.open(ReaderOverlay::Tab::Settings);
            _overlay.redraw();
            return true;
        } else if (event.x > w / 2) {
            next_page();
        } else {
            prev_page();
        }
        save_progress();
        redraw(w, h);
        return true;
    }

private:
    static int overlay_chapter_count(void *)
    {
        return g_epub.toc_count();
    }

    static int overlay_current_chapter(void *context)
    {
        EpubReaderActivity *reader = static_cast<EpubReaderActivity *>(context);
        int current_spine = reader ? reader->current_spine_index() : 0;
        int count = g_epub.toc_count();
        for (int i = 0; i < count; ++i) {
            if (g_epub.toc_item(i).spine_index >= current_spine) return i;
        }
        return std::max(0, count - 1);
    }

    static std::string overlay_chapter_title(void *, int index)
    {
        if (index < 0 || index >= g_epub.toc_count()) return std::string();
        return g_epub.toc_item(index).title;
    }

    static int overlay_chapter_depth(void *, int index)
    {
        if (index < 0 || index >= g_epub.toc_count()) return 0;
        return g_epub.toc_item(index).depth;
    }

    static bool overlay_activate_chapter(void *context, int index)
    {
        EpubReaderActivity *reader = static_cast<EpubReaderActivity *>(context);
        if (!reader || index < 0 || index >= g_epub.toc_count()) return false;
        return reader->jump_to_spine(g_epub.toc_item(index).spine_index);
    }

    void handle_overlay_action(ReaderOverlay::Action action)
    {
        if (action == ReaderOverlay::Action::None) return;
        if (action == ReaderOverlay::Action::OpenFont) {
            launch_reader_font_picker();
            return;
        }

        bool apply_layout = _overlay.settings_dirty();
        _overlay.close();
        if (apply_layout && _ok) {
            refresh_text_renderer();
            repaginate_current_spine();
            _overlay.clear_settings_dirty();
        }
        redraw_all(epd_mode_t::epd_quality);
    }

    using PageItemType = document_layout::PageItemType;
    using PageItem = document_layout::PageItem;

    struct CachedImage {
        std::string path;
        std::string ext;
        int width = 0;
        int height = 0;
        bool jpg_progressive = false;
    };

    struct ImageLoadStats {
        bool memory_cache_hit = false;
        bool file_cache_hit = false;
        int64_t extract_ms = 0;
        int64_t probe_ms = 0;
        int64_t total_ms = 0;
    };

    struct ImageDrawStats {
        bool hardware_attempted = false;
        bool hardware_ok = false;
        bool file_decode_ok = false;
        bool fallback_used = false;
        bool software_attempted = false;
        bool software_ok = false;
        int software_scale_denom = 1;
        uint32_t software_memory_budget = 0;
        uint32_t software_legacy_coefficients = 0;
        uint32_t software_compact_coefficients = 0;
        int64_t hardware_read_ms = 0;
        int64_t hardware_header_ms = 0;
        int64_t hardware_decode_ms = 0;
        int64_t hardware_scale_ms = 0;
        int64_t hardware_push_ms = 0;
        int64_t hardware_total_ms = 0;
        int64_t software_header_ms = 0;
        int64_t software_decode_ms = 0;
        int64_t software_push_ms = 0;
        int64_t software_total_ms = 0;
        int64_t file_decode_ms = 0;
        int64_t fallback_read_ms = 0;
        int64_t fallback_decode_ms = 0;
        int64_t total_ms = 0;
    };

    bool _ok = false;
    bool _seen_show = false;
    int _spine_index = 0;
    int _page_index = 0;
    int _page_count = 0;
    uint32_t _layout_sig = 0;
    std::string _progress_path;
    std::string _image_cache_dir;
    document_layout::Pages _pages;
    std::unordered_map<std::string, CachedImage> _image_cache;

    void release_runtime_cache()
    {
        size_t page_count = _pages.size();
        size_t page_item_count = 0;
        for (const auto &page : _pages) page_item_count += page.size();
        size_t image_count = _image_cache.size();

        document_layout::Pages().swap(_pages);
        std::unordered_map<std::string, CachedImage>().swap(_image_cache);
        _page_count = 0;
        _page_index = 0;

        ESP_LOGI(TAG,
                 "release cache pages=%u items=%u images=%u",
                 (unsigned)page_count,
                 (unsigned)page_item_count,
                 (unsigned)image_count);
    }

    uint32_t layout_signature() const
    {
        std::string sig = reader_text_renderer().layoutSignature();
        struct stat st = {};
        long file_size = -1;
        if (!g_epub.path().empty() && stat(g_epub.path().c_str(), &st) == 0) {
            file_size = (long)st.st_size;
        }

        ReaderStyle style = reader_style_get();
        char buf[192];
        snprintf(buf,
                 sizeof(buf),
                 ";algo=%lu;doc_algo=%lu;w=%d;h=%d;cw=%d;lpp=%d;m=%d,%d,%d,%d;indent=%d;paragraph=%d;spine=%d;file=%ld",
                 (unsigned long)EPUB_LAYOUT_VERSION,
                 (unsigned long)document_layout::ALGORITHM_VERSION,
                 (int)M5.Display.width(),
                 (int)M5.Display.height(),
                 content_width(),
                 lines_per_page(),
                 style.margin_left,
                 style.margin_right,
                 style.margin_top,
                 style.margin_bottom,
                 style.first_line_indent,
                 style.paragraph_spacing,
                 g_epub.spine_count(),
                 file_size);
        sig += buf;
        return hash32_string(sig);
    }

    uint64_t page_position(int page_index) const
    {
        if (page_index < 0 || page_index >= (int)_pages.size()) return 0;
        const auto &page = _pages[(size_t)page_index];
        for (const auto &item : page) {
            if (item.type != PageItemType::Space) return item.content_position;
        }
        return page.empty() ? 0 : page.front().content_position;
    }

    int page_for_position(uint64_t position) const
    {
        if (_pages.empty()) return 0;
        int result = 0;
        for (int i = 0; i < (int)_pages.size(); ++i) {
            uint64_t start = page_position(i);
            if (start > position) break;
            result = i;
        }
        return result;
    }

    bool load_progress(int *spine, uint64_t *position)
    {
        if (spine) *spine = 0;
        if (position) *position = 0;
        if (_progress_path.empty()) return false;

        FILE *fp = fopen(_progress_path.c_str(), "rb");
        if (!fp) return false;

        uint32_t magic = 0, version = 0, layout = 0;
        uint32_t saved_spine = 0;
        uint64_t saved_position = 0;
        bool ok = fread(&magic, 1, sizeof(magic), fp) == sizeof(magic) &&
                  fread(&version, 1, sizeof(version), fp) == sizeof(version) &&
                  fread(&layout, 1, sizeof(layout), fp) == sizeof(layout) &&
                  fread(&saved_spine, 1, sizeof(saved_spine), fp) == sizeof(saved_spine) &&
                  fread(&saved_position, 1, sizeof(saved_position), fp) == sizeof(saved_position);
        fclose(fp);

        if (!ok || magic != EPUB_PROGRESS_MAGIC || version != EPUB_PROGRESS_VERSION) {
            ESP_LOGI(TAG, "progress ignored ok=%d magic=%08lx version=%lu layout=%08lx/%08lx",
                     ok ? 1 : 0,
                     (unsigned long)magic,
                     (unsigned long)version,
                     (unsigned long)layout,
                     (unsigned long)_layout_sig);
            return false;
        }
        if (saved_spine >= (uint32_t)g_epub.spine_count()) return false;

        if (spine) *spine = (int)saved_spine;
        if (position) *position = saved_position;
        ESP_LOGI(TAG, "progress loaded spine=%lu position=%llu layout=%08lx/%08lx",
                 (unsigned long)saved_spine + 1,
                 (unsigned long long)saved_position,
                 (unsigned long)layout,
                 (unsigned long)_layout_sig);
        return true;
    }

    void save_progress()
    {
        if (!_ok || _progress_path.empty()) return;
        ensure_epub_progress_root();

        FILE *fp = fopen(_progress_path.c_str(), "wb");
        if (!fp) return;

        uint32_t magic = EPUB_PROGRESS_MAGIC;
        uint32_t version = EPUB_PROGRESS_VERSION;
        uint32_t spine = (uint32_t)std::max(0, _spine_index);
        uint64_t position = page_position(_page_index);
        fwrite(&magic, 1, sizeof(magic), fp);
        fwrite(&version, 1, sizeof(version), fp);
        fwrite(&_layout_sig, 1, sizeof(_layout_sig), fp);
        fwrite(&spine, 1, sizeof(spine), fp);
        fwrite(&position, 1, sizeof(position), fp);
        fclose(fp);
        ESP_LOGI(TAG, "progress saved spine=%lu position=%llu page=%d/%d",
                 (unsigned long)spine + 1,
                 (unsigned long long)position,
                 _page_index + 1,
                 _page_count);
    }

    int content_x() const
    {
        return reader_style_get().margin_left;
    }

    int content_y() const
    {
        return TOP_H + reader_style_get().margin_top;
    }

    int content_width() const
    {
        ReaderStyle style = reader_style_get();
        int width = M5.Display.width() - style.margin_left - style.margin_right;
        return width > 100 ? width : 100;
    }

    int content_height() const
    {
        ReaderStyle style = reader_style_get();
        int height = M5.Display.height() - TOP_H - style.margin_top - style.margin_bottom;
        return height > 100 ? height : 100;
    }

    int lines_per_page() const
    {
        ReaderStyle style = reader_style_get();
        int line_h = reader_text_renderer().lineHeight();
        if (line_h <= 0) line_h = 48;
        int usable_h = M5.Display.height() - TOP_H - style.margin_top - style.margin_bottom;
        int n = usable_h / line_h;
        return n > 1 ? n : 1;
    }

    bool load_spine(int index)
    {
        if (!_ok || index < 0 || index >= g_epub.spine_count()) return false;

        int64_t t0 = esp_timer_get_time();
        std::string xhtml;
        const auto &spine = g_epub.spine_item(index);
        if (!g_epub.read_item_to_string(spine.href, &xhtml)) {
            ESP_LOGW(TAG, "read spine failed index=%d href=%s", index, spine.href.c_str());
            return false;
        }
        int64_t t_read = esp_timer_get_time();

        std::vector<EpubContentBlock> blocks;
        if (!epub_parse_xhtml_blocks(xhtml, spine.href, &blocks)) {
            ESP_LOGW(TAG, "parse spine failed index=%d href=%s", index, spine.href.c_str());
            return false;
        }
        int64_t t_parse = esp_timer_get_time();

        _spine_index = index;
        _page_index = 0;
        build_pages(blocks);
        int64_t t_layout = esp_timer_get_time();

        ESP_LOGI(TAG, "load_spine index=%d/%d href=%s blocks=%u pages=%d read=%lld parse=%lld layout=%lld total=%lld ms",
                 _spine_index + 1,
                 g_epub.spine_count(),
                 spine.href.c_str(),
                 (unsigned)blocks.size(),
                 _page_count,
                 (t_read - t0) / 1000,
                 (t_parse - t_read) / 1000,
                 (t_layout - t_parse) / 1000,
                 (t_layout - t0) / 1000);
        return true;
    }

    void repaginate_current_spine()
    {
        uint64_t old_position = page_position(_page_index);
        load_spine(_spine_index);
        if (_page_count > 0) _page_index = page_for_position(old_position);
    }

    std::string image_cache_path(const std::string &href) const
    {
        std::string ext = lower_ext(href);
        if (ext != ".jpg" && ext != ".png" && ext != ".bmp") ext = ".img";
        uint32_t h = hash32_string(href);
        char name[40];
        snprintf(name, sizeof(name), "/%08lx%s", (unsigned long)h, ext.c_str());
        return _image_cache_dir + name;
    }

    std::string cover_cache_path(const std::string &href) const
    {
        std::string ext = lower_ext(href);
        if (ext != ".jpg" && ext != ".png" && ext != ".bmp") return std::string();
        return _image_cache_dir + "/cover" + ext;
    }

    std::string ensure_cover_cache()
    {
        std::string href = g_epub.cover_image_href();
        if (href.empty()) return std::string();
        std::string out_path = cover_cache_path(href);
        if (out_path.empty()) return std::string();
        std::string thumbnail_path = cover_thumbnail::cache_path_for(out_path);
        if (file_exists(thumbnail_path)) {
            if (file_exists(out_path)) remove(out_path.c_str());
            return thumbnail_path;
        }
        app_paths::ensure_dir(app_paths::epub_cache_dir());
        mkdir(_image_cache_dir.c_str(), 0);
        if (!file_exists(out_path) && !g_epub.extract_item_to_file(href, out_path)) {
            ESP_LOGW(TAG, "cover extract failed href=%s path=%s", href.c_str(), out_path.c_str());
            remove(out_path.c_str());
            return std::string();
        }
        if (cover_thumbnail::ensure(out_path, thumbnail_path)) {
            remove(out_path.c_str());
            return thumbnail_path;
        }
        ESP_LOGW(TAG, "cover thumbnail failed, retaining original path=%s", out_path.c_str());
        return out_path;
    }

    bool is_supported_image_href(const std::string &href) const
    {
        std::string ext = lower_ext(href);
        return ext == ".jpg" || ext == ".png" || ext == ".bmp";
    }

    bool ensure_cached_image(const std::string &href, CachedImage *image, ImageLoadStats *stats = nullptr)
    {
        int64_t t0 = esp_timer_get_time();
        if (stats) *stats = ImageLoadStats();
        if (!image || href.empty()) return false;
        auto cached = _image_cache.find(href);
        if (cached != _image_cache.end() && file_exists(cached->second.path)) {
            *image = cached->second;
            if (stats) {
                stats->memory_cache_hit = true;
                stats->file_cache_hit = true;
                stats->total_ms = (esp_timer_get_time() - t0) / 1000;
            }
            return true;
        }

        std::string ext = lower_ext(href);
        if (ext != ".jpg" && ext != ".png" && ext != ".bmp") {
            ESP_LOGW(TAG, "unsupported image ext href=%s", href.c_str());
            return false;
        }

        app_paths::ensure_dir(app_paths::epub_cache_dir());
        mkdir(_image_cache_dir.c_str(), 0);
        std::string out_path = image_cache_path(href);
        bool file_cached = file_exists(out_path);
        if (!file_cached) {
            int64_t t_extract = esp_timer_get_time();
            if (!g_epub.extract_item_to_file(href, out_path)) {
                ESP_LOGW(TAG, "image extract failed href=%s", href.c_str());
                return false;
            }
            if (stats) stats->extract_ms = (esp_timer_get_time() - t_extract) / 1000;
        }

        CachedImage info;
        info.path = out_path;
        info.ext = ext;
        int64_t t_probe = esp_timer_get_time();
        if (!probe_image_size(out_path, ext, &info.width, &info.height, &info.jpg_progressive)) {
            ESP_LOGW(TAG, "image probe failed href=%s path=%s", href.c_str(), out_path.c_str());
            return false;
        }
        if (stats) {
            stats->file_cache_hit = file_cached;
            stats->probe_ms = (esp_timer_get_time() - t_probe) / 1000;
            stats->total_ms = (esp_timer_get_time() - t0) / 1000;
        }

        _image_cache[href] = info;
        *image = info;
        return true;
    }

    void fit_image_box(int src_w, int src_h, int max_w, int max_h, int *draw_w, int *draw_h, float *scale) const
    {
        document_layout::fit_image_box(src_w, src_h, max_w, max_h, false,
                                       draw_w, draw_h, scale);
    }

    bool read_file_bytes(const std::string &path, std::vector<uint8_t> *out, size_t max_size = 12 * 1024 * 1024)
    {
        if (!out) return false;
        out->clear();

        struct stat st = {};
        if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > max_size) return false;

        FILE *fp = fopen(path.c_str(), "rb");
        if (!fp) return false;
        out->resize((size_t)st.st_size);
        bool ok = fread(out->data(), 1, out->size(), fp) == out->size();
        fclose(fp);
        if (!ok) out->clear();
        return ok;
    }

    bool draw_cached_image(const CachedImage &image, int x, int y, int w, int h, float scale, ImageDrawStats *stats = nullptr)
    {
        int64_t t0 = esp_timer_get_time();
        if (stats) *stats = ImageDrawStats();
        bool ok = false;
        if (image.ext == ".jpg") {
            if (image.jpg_progressive) {
                turbo_jpeg::DrawStats sw;
                ok = turbo_jpeg::draw_file(M5.Display, image.path.c_str(), x, y, w, h, &sw);
                if (stats) {
                    stats->software_attempted = sw.attempted;
                    stats->software_ok = ok;
                    stats->software_scale_denom = sw.scale_denom;
                    stats->software_memory_budget = sw.memory_budget;
                    stats->software_legacy_coefficients = sw.legacy_coefficient_bytes;
                    stats->software_compact_coefficients = sw.compact_coefficient_bytes;
                    stats->software_header_ms = sw.header_ms;
                    stats->software_decode_ms = sw.decode_ms;
                    stats->software_push_ms = sw.push_ms;
                    stats->software_total_ms = sw.total_ms;
                }
                if (stats) stats->total_ms = (esp_timer_get_time() - t0) / 1000;
                return ok;
            }
            p4_jpeg::DrawStats hw;
            ok = p4_jpeg::draw_file(M5.Display, image.path.c_str(), x, y, w, h, &hw);
            if (stats) {
                stats->hardware_attempted = hw.attempted;
                stats->hardware_ok = ok;
                stats->hardware_read_ms = hw.read_ms;
                stats->hardware_header_ms = hw.header_ms;
                stats->hardware_decode_ms = hw.decode_ms;
                stats->hardware_scale_ms = hw.scale_ms;
                stats->hardware_push_ms = hw.push_ms;
                stats->hardware_total_ms = hw.total_ms;
            }
            int64_t t_file = esp_timer_get_time();
            if (!ok) {
                ok = M5.Display.drawJpgFile(image.path.c_str(), x, y, w, h, 0, 0, scale, scale);
                if (stats) stats->file_decode_ms = (esp_timer_get_time() - t_file) / 1000;
                if (ok && stats) stats->file_decode_ok = true;
            }
            if (!ok) {
                std::vector<uint8_t> bytes;
                int64_t t_read = esp_timer_get_time();
                if (read_file_bytes(image.path, &bytes)) {
                    if (stats) {
                        stats->fallback_used = true;
                        stats->fallback_read_ms = (esp_timer_get_time() - t_read) / 1000;
                    }
                    int64_t t_decode = esp_timer_get_time();
                    ok = M5.Display.drawJpg(bytes.data(), bytes.size(), x, y, w, h, 0, 0, scale, scale);
                    if (stats) stats->fallback_decode_ms = (esp_timer_get_time() - t_decode) / 1000;
                }
            }
        } else if (image.ext == ".png") {
            int64_t t_file = esp_timer_get_time();
            ok = M5.Display.drawPngFile(image.path.c_str(), x, y, w, h, 0, 0, scale, scale);
            if (stats) stats->file_decode_ms = (esp_timer_get_time() - t_file) / 1000;
            if (ok && stats) stats->file_decode_ok = true;
            if (!ok) {
                std::vector<uint8_t> bytes;
                int64_t t_read = esp_timer_get_time();
                if (read_file_bytes(image.path, &bytes)) {
                    if (stats) {
                        stats->fallback_used = true;
                        stats->fallback_read_ms = (esp_timer_get_time() - t_read) / 1000;
                    }
                    int64_t t_decode = esp_timer_get_time();
                    ok = M5.Display.drawPng(bytes.data(), bytes.size(), x, y, w, h, 0, 0, scale, scale);
                    if (stats) stats->fallback_decode_ms = (esp_timer_get_time() - t_decode) / 1000;
                }
            }
        } else if (image.ext == ".bmp") {
            int64_t t_file = esp_timer_get_time();
            ok = M5.Display.drawBmpFile(image.path.c_str(), x, y, w, h, 0, 0, scale, scale);
            if (stats) stats->file_decode_ms = (esp_timer_get_time() - t_file) / 1000;
            if (ok && stats) stats->file_decode_ok = true;
            if (!ok) {
                std::vector<uint8_t> bytes;
                int64_t t_read = esp_timer_get_time();
                if (read_file_bytes(image.path, &bytes)) {
                    if (stats) {
                        stats->fallback_used = true;
                        stats->fallback_read_ms = (esp_timer_get_time() - t_read) / 1000;
                    }
                    int64_t t_decode = esp_timer_get_time();
                    ok = M5.Display.drawBmp(bytes.data(), bytes.size(), x, y, w, h, 0, 0, scale, scale);
                    if (stats) stats->fallback_decode_ms = (esp_timer_get_time() - t_decode) / 1000;
                }
            }
        }
        if (stats) stats->total_ms = (esp_timer_get_time() - t0) / 1000;
        return ok;
    }

    void draw_image_placeholder(int x, int y, int w, int h, const char *message)
    {
        int box_h = std::min(h, 120);
        int yy = y + std::max(0, (h - box_h) / 2);
        M5.Display.drawRect(x, yy, w, box_h, TFT_BLACK);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        system_font::draw_center(M5.Display, message, x + w / 2, yy + 32);
    }

    void build_pages(std::vector<EpubContentBlock> &blocks)
    {
        auto &renderer = reader_text_renderer();
        int line_h = renderer.lineHeight();
        if (line_h <= 0) line_h = 48;

        std::vector<document_layout::Block> document_blocks;
        document_blocks.reserve(blocks.size());
        uint64_t content_position = 0;
        for (auto &block : blocks) {
            document_layout::Block output;
            bool is_image = block.type == EpubContentBlockType::Image;
            if (block.type == EpubContentBlockType::Image) {
                output.type = document_layout::BlockType::Image;
            } else if (block.type == EpubContentBlockType::HorizontalRule) {
                output.type = document_layout::BlockType::HorizontalRule;
            } else if (block.type == EpubContentBlockType::PageBreak) {
                output.type = document_layout::BlockType::PageBreak;
            } else {
                output.type = document_layout::BlockType::Paragraph;
            }
            output.text = std::move(block.text);
            output.resource = std::move(block.href);
            output.content_position = content_position;
            output.image_width = block.image_width;
            output.image_height = block.image_height;
            output.blank_before = block.blank_before;
            output.blank_after = block.blank_after;
            output.heading = block.heading;
            output.heading_level = block.heading_level;
            output.page_break_before = block.page_break_before;
            output.allow_first_line_indent = !block.heading &&
                                             block.type == EpubContentBlockType::Text;
            output.paragraph_spacing_after = output.allow_first_line_indent;
            size_t text_bytes = output.text.size();
            document_blocks.push_back(std::move(output));

            if (is_image) {
                content_position += 1;
            } else {
                content_position += text_bytes + 1;
            }
        }

        document_layout::Options options;
        options.content_width = content_width();
        options.content_height = content_height();
        options.line_height = line_h;
        ReaderStyle reader_style = reader_style_get();
        int fullwidth_advance = renderer.measureFullwidthCellForLayout();
        options.first_line_indent = fullwidth_advance * reader_style.first_line_indent;
        options.paragraph_spacing_percent = reader_style.paragraph_spacing * 10;
        options.layout_paragraph = [](void *context, const std::string &text, int width, int indent) {
            return static_cast<TextRenderer *>(context)->layoutUtf8Paragraph(text, width, indent);
        };
        options.text_context = &renderer;
        options.resolve_image = [](void *context,
                                   const std::string &resource,
                                   document_layout::ImageInfo *info) {
            auto *reader = static_cast<EpubReaderActivity *>(context);
            if (!reader || !info || !reader->is_supported_image_href(resource)) {
                ESP_LOGW(TAG, "unsupported image href=%s", resource.c_str());
                return false;
            }
            CachedImage image;
            if (!reader->ensure_cached_image(resource, &image)) {
                ESP_LOGW(TAG, "image layout probe failed href=%s", resource.c_str());
                return false;
            }
            info->width = image.width;
            info->height = image.height;
            return true;
        };
        options.image_context = this;
        _pages = document_layout::paginate(document_blocks, options);

        for (const auto &page : _pages) {
            for (const auto &item : page) {
                if (item.type != PageItemType::Image) continue;
                ESP_LOGI(TAG,
                         "image layout href=%s src=%dx%d declared=%dx%d box=%dx%d scale=%.3f remaining=%d/%d fit_current=%d",
                         item.resource.c_str(), item.source_width, item.source_height,
                         item.declared_width, item.declared_height,
                         item.width, item.height, (double)item.scale,
                         item.remaining_height, content_height(),
                         item.fitted_current_page ? 1 : 0);
            }
        }

        _page_count = (int)_pages.size();
        _page_index = std::min(_page_index, std::max(0, _page_count - 1));
    }

    bool first_image_on_page(int page_index, std::string *href) const
    {
        if (!href || page_index < 0 || page_index >= (int)_pages.size()) return false;
        for (const auto &item : _pages[(size_t)page_index]) {
            if (item.type == PageItemType::Image && is_supported_image_href(item.resource)) {
                *href = item.resource;
                return true;
            }
        }
        return false;
    }

    bool first_image_in_spine(int spine_index, std::string *href)
    {
        if (!href || spine_index < 0 || spine_index >= g_epub.spine_count()) return false;

        std::string xhtml;
        const auto &spine = g_epub.spine_item(spine_index);
        if (!g_epub.read_item_to_string(spine.href, &xhtml)) return false;

        std::vector<EpubContentBlock> blocks;
        if (!epub_parse_xhtml_blocks(xhtml, spine.href, &blocks)) return false;
        for (const auto &block : blocks) {
            if (block.type == EpubContentBlockType::Image && is_supported_image_href(block.href)) {
                *href = block.href;
                return true;
            }
        }
        return false;
    }

    bool next_image_href(std::string *href)
    {
        if (!href) return false;
        href->clear();

        for (int i = _page_index + 1; i < (int)_pages.size(); ++i) {
            if (first_image_on_page(i, href)) return true;
        }

        const int max_spines_to_scan = 3;
        for (int i = 1; i <= max_spines_to_scan; ++i) {
            int spine = _spine_index + i;
            if (spine >= g_epub.spine_count()) break;
            if (first_image_in_spine(spine, href)) return true;
        }
        return false;
    }

    void prefetch_next_image()
    {
        if (!_ok) return;

        std::string href;
        if (!next_image_href(&href) || href.empty()) return;

        app_paths::ensure_dir(app_paths::epub_cache_dir());
        mkdir(_image_cache_dir.c_str(), 0);

        std::string out_path = image_cache_path(href);
        if (file_exists(out_path)) {
            ESP_LOGI(TAG, "image prefetch href=%s file_cache=1", href.c_str());
            return;
        }

        int64_t t0 = esp_timer_get_time();
        bool ok = g_epub.extract_item_to_file(href, out_path);
        ESP_LOGI(TAG,
                 "image prefetch href=%s ok=%d extract=%lld path=%s",
                 href.c_str(),
                 ok ? 1 : 0,
                 (esp_timer_get_time() - t0) / 1000,
                 out_path.c_str());
    }

    void next_page()
    {
        if (!_ok) return;
        if (_page_index + 1 < _page_count) {
            _page_index++;
            return;
        }

        int next_spine = _spine_index + 1;
        while (next_spine < g_epub.spine_count()) {
            if (load_spine(next_spine) && !_pages.empty()) return;
            next_spine++;
        }
    }

    void prev_page()
    {
        if (!_ok) return;
        if (_page_index > 0) {
            _page_index--;
            return;
        }

        int prev_spine = _spine_index - 1;
        while (prev_spine >= 0) {
            if (load_spine(prev_spine)) {
                _page_index = std::max(0, _page_count - 1);
                return;
            }
            prev_spine--;
        }
    }

    void draw_topbar(int w)
    {
        M5.Display.fillRect(0, 0, w, TOP_H, TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

        std::string title = _ok ? short_title(g_epub.title) : "EPUB open failed";
        system_font::draw_center(M5.Display, system_font::ellipsize(title, w - 180), w / 2, 3);

        if (_ok) {
            char pg[32];
            snprintf(pg, sizeof(pg), "%d/%d", _spine_index + 1, g_epub.spine_count());
            system_font::draw_right(M5.Display, pg, w - 5, 3);
        }
    }

    void draw_content(int w, int h)
    {
        (void)w;
        if (!_ok) {
            system_font::draw_left(M5.Display, "Could not parse EPUB container/OPF.", 20, TOP_H + 18);
            return;
        }

        if (_page_index < 0 || _page_index >= (int)_pages.size()) return;
        const auto &page = _pages[(size_t)_page_index];
        int x = content_x();
        int y = content_y();
        int line_h = reader_text_renderer().lineHeight();
        if (line_h <= 0) line_h = 48;

        std::string page_text;
        for (const auto &item : page) {
            if (item.type == PageItemType::Text && !item.text.empty()) {
                page_text += item.text;
                page_text += '\n';
            }
        }
        if (!page_text.empty()) {
            bool prepared = reader_text_renderer().preparePage(page_text);
            if (!prepared) ESP_LOGW(TAG, "preparePage reported missing glyphs or render errors");
        }

        for (size_t i = 0; i < page.size() && y < h; ++i) {
            if ((i & 7) == 0) vTaskDelay(1);
            const PageItem &item = page[i];
            if (item.type == PageItemType::Text) {
                if (!item.text.empty()) {
                    int text_x = x + item.x_offset;
                    if (item.centered) {
                        int text_width = reader_text_renderer().measureUtf8(item.text);
                        text_x = x + std::max(0, (content_width() - text_width) / 2);
                    }
                    reader_text_renderer().drawUtf8Line(
                        M5.Display, item.text, text_x, y, TFT_BLACK,
                        item.justify ? item.width : 0);
                }
                y += line_h;
            } else if (item.type == PageItemType::Space) {
                y += item.height > 0 ? item.height : line_h;
            } else if (item.type == PageItemType::HorizontalRule) {
                int rule_y = y + item.height / 2;
                int inset = std::max(20, content_width() / 8);
                M5.Display.drawFastHLine(x + inset, rule_y,
                                         std::max(1, content_width() - inset * 2), TFT_BLACK);
                y += item.height;
            } else if (item.type == PageItemType::Image) {
                CachedImage image;
                ImageLoadStats load_stats;
                if (!ensure_cached_image(item.resource, &image, &load_stats)) {
                    system_font::draw_left(M5.Display, "[image]", x, y);
                    y += item.height;
                    continue;
                }

                int draw_w = item.width;
                int draw_h = item.height;
                float scale = item.scale;
                fit_image_box(image.width, image.height,
                              item.width > 0 ? item.width : content_width(),
                              item.height, &draw_w, &draw_h, &scale);
                int draw_x = x + std::max(0, (content_width() - draw_w) / 2);
                int draw_y = y + std::max(0, (item.height - draw_h) / 2);
                ImageDrawStats draw_stats;
                bool ok = draw_cached_image(image, draw_x, draw_y, draw_w, draw_h, scale, &draw_stats);
                ESP_LOGI(TAG,
                         "image timing href=%s ext=%s src=%dx%d draw=%dx%d scale=%.3f mem_cache=%d file_cache=%d extract=%lld probe=%lld load=%lld hw_try=%d hw_ok=%d hw_read=%lld hw_header=%lld hw_decode=%lld hw_scale=%lld hw_push=%lld hw_total=%lld sw_try=%d sw_ok=%d sw_denom=%d sw_budget=%u sw_coeff=%u/%u sw_header=%lld sw_decode=%lld sw_push=%lld sw_total=%lld file_draw=%lld fallback=%d fallback_read=%lld fallback_draw=%lld draw=%lld ok=%d progressive=%d",
                         item.resource.c_str(),
                         image.ext.c_str(),
                         image.width,
                         image.height,
                         draw_w,
                         draw_h,
                         (double)scale,
                         load_stats.memory_cache_hit ? 1 : 0,
                         load_stats.file_cache_hit ? 1 : 0,
                         load_stats.extract_ms,
                         load_stats.probe_ms,
                         load_stats.total_ms,
                         draw_stats.hardware_attempted ? 1 : 0,
                         draw_stats.hardware_ok ? 1 : 0,
                         draw_stats.hardware_read_ms,
                         draw_stats.hardware_header_ms,
                         draw_stats.hardware_decode_ms,
                         draw_stats.hardware_scale_ms,
                         draw_stats.hardware_push_ms,
                         draw_stats.hardware_total_ms,
                         draw_stats.software_attempted ? 1 : 0,
                         draw_stats.software_ok ? 1 : 0,
                         draw_stats.software_scale_denom,
                         (unsigned)draw_stats.software_memory_budget,
                         (unsigned)draw_stats.software_compact_coefficients,
                         (unsigned)draw_stats.software_legacy_coefficients,
                         draw_stats.software_header_ms,
                         draw_stats.software_decode_ms,
                         draw_stats.software_push_ms,
                         draw_stats.software_total_ms,
                         draw_stats.file_decode_ms,
                         draw_stats.fallback_used ? 1 : 0,
                         draw_stats.fallback_read_ms,
                         draw_stats.fallback_decode_ms,
                         draw_stats.total_ms,
                         ok ? 1 : 0,
                         image.jpg_progressive ? 1 : 0);
                if (!ok) {
                    draw_image_placeholder(draw_x, draw_y, draw_w, draw_h,
                                           image.jpg_progressive ? "Progressive JPEG too large" : "Image decode failed");
                    ESP_LOGW(TAG,
                             "image draw failed path=%s ext=%s src=%dx%d draw=%dx%d scale=%.3f progressive=%d",
                             image.path.c_str(),
                             image.ext.c_str(),
                             image.width,
                             image.height,
                             draw_w,
                             draw_h,
                             (double)scale,
                             image.jpg_progressive ? 1 : 0);
                }
                y += item.height;
            }
        }
    }

    void redraw(int w, int h)
    {
        int64_t t0 = esp_timer_get_time();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epub_reader_epd_mode());
        M5.Display.fillRect(0, TOP_H, w, h - TOP_H, TFT_WHITE);
        draw_topbar(w);
        draw_content(w, h);
        if (_overlay.visible()) _overlay.draw();
        M5.Display.display();
        M5.Display.endWrite();
        ESP_LOGI(TAG, "redraw spine=%d page=%d/%d total=%lld ms",
                 _spine_index + 1,
                 _page_index + 1,
                 _page_count,
                 (esp_timer_get_time() - t0) / 1000);
        prefetch_next_image();
    }

    void redraw_all(epd_mode_t mode = epub_reader_epd_mode())
    {
        int w = M5.Display.width();
        int h = M5.Display.height();
        int64_t t0 = esp_timer_get_time();
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar(w);
        draw_content(w, h);
        if (_overlay.visible()) _overlay.draw();
        M5.Display.display();
        M5.Display.endWrite();
        epd_mode_t default_mode = epub_reader_epd_mode();
        if (mode != default_mode) M5.Display.setEpdMode(default_mode);
        ESP_LOGI(TAG, "redraw_all spine=%d page=%d/%d total=%lld ms",
                 _spine_index + 1,
                 _page_index + 1,
                 _page_count,
                 (esp_timer_get_time() - t0) / 1000);
        prefetch_next_image();
    }

    ReaderOverlay _overlay;
};

void launch_epub_reader(const char *path)
{
    ActivityManager::instance().push(new EpubReaderActivity(path));
}
