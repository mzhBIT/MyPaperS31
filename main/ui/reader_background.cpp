#include "ui/reader_background.h"
#include "ui/standby_wallpaper.h"
#include "ui/reader_book_cover.h"
#include "ui/UiLocale.h"
#include "renderer/TurboJpegDecoder.h"

#include "storage/app_paths.h"

#include <M5Unified.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
struct Settings {

const char *NVS_NAMESPACE = "reader_bg";
static constexpr const char *NVS_PATH_KEY = "path";
const char *CACHE_PATH = "/sdcard/.reader/reader_background.g4";
const char *CACHE_TEMP_PATH = "/sdcard/.reader/reader_background.g4.tmp";
static constexpr const char *TAG = "READER_BG";
static constexpr uint32_t CACHE_MAGIC = 0x34474252; // RBG4
static constexpr uint32_t CACHE_VERSION = 2;
static constexpr lgfx::bgr888_t GRAYSCALE_PALETTE[16] = {
    {0x00, 0x00, 0x00}, {0x11, 0x11, 0x11},
    {0x22, 0x22, 0x22}, {0x33, 0x33, 0x33},
    {0x44, 0x44, 0x44}, {0x55, 0x55, 0x55},
    {0x66, 0x66, 0x66}, {0x77, 0x77, 0x77},
    {0x88, 0x88, 0x88}, {0x99, 0x99, 0x99},
    {0xaa, 0xaa, 0xaa}, {0xbb, 0xbb, 0xbb},
    {0xcc, 0xcc, 0xcc}, {0xdd, 0xdd, 0xdd},
    {0xee, 0xee, 0xee}, {0xff, 0xff, 0xff},
};

struct CacheHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t payload_size;
    uint32_t source_size;
    uint32_t source_mtime;
    uint32_t path_hash;
};

bool initialized = false;
std::string background_path;
std::string background_ext;
int background_width = 0;
int background_height = 0;
bool background_progressive = false;
bool require_full_resolution = true;
uint8_t *background_cache = nullptr;
size_t background_cache_size = 0;
int background_cache_width = 0;
int background_cache_height = 0;

void release_cache()
{
    if (background_cache) heap_caps_free(background_cache);
    background_cache = nullptr;
    background_cache_size = 0;
    background_cache_width = 0;
    background_cache_height = 0;
}

uint32_t path_hash(const std::string &path)
{
    uint32_t value = 2166136261u;
    for (uint8_t c : path) {
        value ^= c;
        value *= 16777619u;
    }
    return value;
}

size_t packed_size(int width, int height)
{
    if (width <= 0 || height <= 0) return 0;
    size_t stride = ((size_t)width + 1u) & ~1u;
    return stride * (size_t)height / 2u;
}

bool source_info(const std::string &path, uint32_t *size, uint32_t *mtime)
{
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return false;
    if (size) *size = (uint32_t)st.st_size;
    if (mtime) *mtime = (uint32_t)st.st_mtime;
    return true;
}

uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

std::string image_extension(const std::string &path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    if (ext == ".jpeg") ext = ".jpg";
    if (ext != ".jpg" && ext != ".png" && ext != ".bmp") return {};
    return ext;
}

bool probe_png(FILE *fp, int *width, int *height)
{
    uint8_t header[24];
    if (std::fseek(fp, 0, SEEK_SET) != 0 ||
        std::fread(header, 1, sizeof(header), fp) != sizeof(header)) return false;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (std::memcmp(header, signature, sizeof(signature)) != 0 ||
        std::memcmp(header + 12, "IHDR", 4) != 0) return false;
    *width = (int)(((uint32_t)header[16] << 24) | ((uint32_t)header[17] << 16) |
                   ((uint32_t)header[18] << 8) | header[19]);
    *height = (int)(((uint32_t)header[20] << 24) | ((uint32_t)header[21] << 16) |
                    ((uint32_t)header[22] << 8) | header[23]);
    return *width > 0 && *height > 0;
}

bool probe_bmp(FILE *fp, int *width, int *height)
{
    uint8_t header[26];
    if (std::fseek(fp, 0, SEEK_SET) != 0 ||
        std::fread(header, 1, sizeof(header), fp) != sizeof(header)) return false;
    if (header[0] != 'B' || header[1] != 'M') return false;
    int32_t w = (int32_t)le32(header + 18);
    int32_t h = (int32_t)le32(header + 22);
    if (w == 0 || h == 0) return false;
    *width = std::abs(w);
    *height = std::abs(h);
    return true;
}

bool probe_jpeg(FILE *fp, int *width, int *height, bool *progressive)
{
    *progressive = false;
    if (std::fseek(fp, 0, SEEK_SET) != 0 ||
        std::fgetc(fp) != 0xff || std::fgetc(fp) != 0xd8) return false;
    while (true) {
        int marker = std::fgetc(fp);
        while (marker == 0xff) marker = std::fgetc(fp);
        if (marker == EOF || marker == 0xd9 || marker == 0xda) return false;
        uint8_t length_bytes[2];
        if (std::fread(length_bytes, 1, 2, fp) != 2) return false;
        int length = be16(length_bytes);
        if (length < 2) return false;
        bool sof = (marker >= 0xc0 && marker <= 0xc3) ||
                   (marker >= 0xc5 && marker <= 0xc7) ||
                   (marker >= 0xc9 && marker <= 0xcb) ||
                   (marker >= 0xcd && marker <= 0xcf);
        if (sof) {
            uint8_t dimensions[5];
            if (std::fread(dimensions, 1, sizeof(dimensions), fp) != sizeof(dimensions)) return false;
            *height = be16(dimensions + 1);
            *width = be16(dimensions + 3);
            *progressive = marker == 0xc2 || marker == 0xc6 ||
                           marker == 0xca || marker == 0xce;
            return *width > 0 && *height > 0;
        }
        if (std::fseek(fp, length - 2, SEEK_CUR) != 0) return false;
    }
}

bool probe_image(const std::string &path, const std::string &ext,
                 int *width, int *height, bool *progressive)
{
    *width = 0;
    *height = 0;
    *progressive = false;
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return false;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = ext == ".jpg" ? probe_jpeg(fp, width, height, progressive)
              : ext == ".png" ? probe_png(fp, width, height)
              : ext == ".bmp" ? probe_bmp(fp, width, height)
                                : false;
    std::fclose(fp);
    return ok;
}

bool draw_source(lgfx::LGFXBase &gfx, const std::string &path,
                 const std::string &ext, int source_width, int source_height,
                 int width, int height, bool clear_background = true)
{
    if (clear_background) gfx.fillScreen(TFT_WHITE);
    float scale = std::max((float)width / source_width,
                           (float)height / source_height);
    int draw_width = std::max(1, (int)std::lround(source_width * scale));
    int draw_height = std::max(1, (int)std::lround(source_height * scale));
    int x = (width - draw_width) / 2;
    int y = (height - draw_height) / 2;

    gfx.setClipRect(0, 0, width, height);
    bool ok = ext == ".jpg"
                  ? gfx.drawJpgFile(path.c_str(), x, y,
                                    draw_width, draw_height, 0, 0, scale, scale)
              : ext == ".png"
                  ? gfx.drawPngFile(path.c_str(), x, y,
                                    draw_width, draw_height, 0, 0, scale, scale)
              : ext == ".bmp"
                  ? gfx.drawBmpFile(path.c_str(), x, y,
                                    draw_width, draw_height, 0, 0, scale, scale)
                  : false;
    if (!ok && !require_full_resolution && ext == ".jpg")
        ok = turbo_jpeg::draw_file(gfx, path.c_str(), x, y, draw_width, draw_height);
    gfx.clearClipRect();
    if (!ok && clear_background) gfx.fillScreen(TFT_WHITE);
    return ok;
}

bool write_cache_file(const std::string &path, int width, int height,
                      const uint8_t *data, size_t size)
{
    uint32_t source_size = 0;
    uint32_t source_mtime = 0;
    if (!source_info(path, &source_size, &source_mtime)) return false;
    CacheHeader header = {
        CACHE_MAGIC, CACHE_VERSION, (uint32_t)width, (uint32_t)height,
        (uint32_t)size, source_size, source_mtime, path_hash(path),
    };
    app_paths::ensure_data_dir();
    FILE *fp = std::fopen(CACHE_TEMP_PATH, "wb");
    if (!fp) return false;
    bool ok = std::fwrite(&header, 1, sizeof(header), fp) == sizeof(header) &&
              std::fwrite(data, 1, size, fp) == size &&
              std::fflush(fp) == 0;
    std::fclose(fp);
    if (!ok) {
        unlink(CACHE_TEMP_PATH);
        return false;
    }
    unlink(CACHE_PATH);
    if (std::rename(CACHE_TEMP_PATH, CACHE_PATH) != 0) {
        unlink(CACHE_TEMP_PATH);
        return false;
    }
    return true;
}

void update_cache_identity(const std::string &path)
{
    uint32_t source_size = 0;
    uint32_t source_mtime = 0;
    if (!source_info(path, &source_size, &source_mtime)) return;
    FILE *fp = std::fopen(CACHE_PATH, "r+b");
    if (!fp) return;
    CacheHeader header = {};
    bool valid = std::fread(&header, 1, sizeof(header), fp) == sizeof(header) &&
                 header.magic == CACHE_MAGIC && header.version == CACHE_VERSION;
    if (valid) {
        header.source_size = source_size;
        header.source_mtime = source_mtime;
        header.path_hash = path_hash(path);
        valid = std::fseek(fp, 0, SEEK_SET) == 0 &&
                std::fwrite(&header, 1, sizeof(header), fp) == sizeof(header) &&
                std::fflush(fp) == 0;
    }
    std::fclose(fp);
    if (!valid) ESP_LOGW(TAG, "cache identity update failed after rename");
}

bool generate_cache(const std::string &path, const std::string &ext,
                    int source_width, int source_height, int width, int height,
                    lgfx::LGFXBase *reading_page = nullptr)
{
    int64_t started = esp_timer_get_time();
    size_t size = packed_size(width, height);
    if (size == 0) return false;

    lgfx::LGFX_Sprite sprite(&M5.Display);
    sprite.setPsram(true);
    // M5GFX treats sub-8-bit sprites as palette-index buffers. Rendering a
    // decoded RGB image directly into one therefore does not produce stable
    // luminance values. Decode into an explicit 8-bit grayscale surface and
    // pack the high nibbles ourselves so the on-disk format is unambiguous.
    sprite.setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    if (!sprite.createSprite(width, height)) {
        ESP_LOGE(TAG, "cache sprite allocation failed: %dx%d", width, height);
        return false;
    }
    if (reading_page) {
        if (!reading_page->isReadable() || reading_page->width() != width ||
            reading_page->height() != height) return false;
        // readRect applies the current screen rotation and writes explicit
        // grayscale pixels directly into the offscreen surface (no RGB buffer).
        static_assert(sizeof(lgfx::grayscale_t) == 1, "8-bit grayscale required");
        reading_page->readRect(0, 0, width, height,
            static_cast<lgfx::grayscale_t *>(sprite.getBuffer()));
    }
    sprite.startWrite();
    bool decoded = draw_source(sprite, path, ext, source_width, source_height,
                               width, height, reading_page == nullptr);
    sprite.endWrite();
    if (!decoded || !sprite.getBuffer()) return false;

    uint8_t *cache = static_cast<uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!cache) {
        ESP_LOGE(TAG, "resident cache allocation failed: %u bytes", (unsigned)size);
        return false;
    }
    const uint8_t *source = static_cast<const uint8_t *>(sprite.getBuffer());
    size_t destination = 0;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = source + (size_t)y * (size_t)width;
        for (int x = 0; x < width; x += 2) {
            uint8_t high = row[x] & 0xf0;
            uint8_t low = x + 1 < width ? (row[x + 1] >> 4) : 0x0f;
            cache[destination++] = high | low;
        }
    }
    // A composited page is specific to this standby operation. Never persist
    // book text or reuse the composition after the user has turned a page.
    if (!reading_page && !write_cache_file(path, width, height, cache, size)) {
        heap_caps_free(cache);
        ESP_LOGE(TAG, "cache file write failed: %s", CACHE_PATH);
        return false;
    }

    release_cache();
    background_cache = cache;
    background_cache_size = size;
    background_cache_width = width;
    background_cache_height = height;
    ESP_LOGI(TAG, "cache generated: %dx%d bytes=%u time=%lld ms source=%s",
             width, height, (unsigned)size,
             (esp_timer_get_time() - started) / 1000, path.c_str());
    return true;
}

bool load_cache(const std::string &path, int width, int height)
{
    size_t expected = packed_size(width, height);
    uint32_t source_size = 0;
    uint32_t source_mtime = 0;
    if (expected == 0 || !source_info(path, &source_size, &source_mtime)) return false;
    FILE *fp = std::fopen(CACHE_PATH, "rb");
    if (!fp) return false;
    CacheHeader header = {};
    bool valid = std::fread(&header, 1, sizeof(header), fp) == sizeof(header) &&
                 header.magic == CACHE_MAGIC && header.version == CACHE_VERSION &&
                 header.width == (uint32_t)width && header.height == (uint32_t)height &&
                 header.payload_size == expected && header.source_size == source_size &&
                 header.source_mtime == source_mtime && header.path_hash == path_hash(path);
    uint8_t *cache = nullptr;
    if (valid) {
        cache = static_cast<uint8_t *>(
            heap_caps_malloc(expected, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        valid = cache && std::fread(cache, 1, expected, fp) == expected;
    }
    std::fclose(fp);
    if (!valid) {
        if (cache) heap_caps_free(cache);
        return false;
    }
    release_cache();
    background_cache = cache;
    background_cache_size = expected;
    background_cache_width = width;
    background_cache_height = height;
    ESP_LOGI(TAG, "cache loaded: %dx%d bytes=%u", width, height, (unsigned)expected);
    return true;
}

bool ensure_cache(int width, int height)
{
    size_t expected = packed_size(width, height);
    if (background_cache && background_cache_size == expected &&
        background_cache_width == width && background_cache_height == height) return true;
    if (load_cache(background_path, width, height)) return true;
    return generate_cache(background_path, background_ext,
                          background_width, background_height, width, height);
}

bool save_path(const std::string &path)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    if (path.empty()) err = nvs_erase_key(handle, NVS_PATH_KEY);
    else err = nvs_set_str(handle, NVS_PATH_KEY, path.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND && path.empty()) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

void ensure_initialized()
{
    if (initialized) return;
    initialized = true;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = 0;
    if (nvs_get_str(handle, NVS_PATH_KEY, nullptr, &length) == ESP_OK && length > 1) {
        std::vector<char> value(length);
        if (nvs_get_str(handle, NVS_PATH_KEY, value.data(), &length) == ESP_OK) {
            background_path = value.data();
            background_ext = image_extension(background_path);
            if (background_ext.empty() ||
                !probe_image(background_path, background_ext,
                             &background_width, &background_height,
                             &background_progressive)) {
                background_path.clear();
                background_ext.clear();
                background_width = 0;
                background_height = 0;
            }
        }
    }
    nvs_close(handle);
}


bool reader_background_enabled()
{
    ensure_initialized();
    return !background_path.empty();
}

const char *reader_background_path()
{
    ensure_initialized();
    return background_path.c_str();
}

ReaderBackgroundResult reader_background_set(const char *path,
                                             int screen_width,
                                             int screen_height,
                                             std::string *message)
{
    ensure_initialized();
    std::string selected = path ? path : "";
    std::string ext = image_extension(selected);
    if (ext.empty()) {
        if (message) *message = ui_locale::text(ui_locale::TextId::ImageFormatsHint);
        return ReaderBackgroundResult::Unsupported;
    }
    int width = 0;
    int height = 0;
    bool progressive = false;
    if (!probe_image(selected, ext, &width, &height, &progressive)) {
        if (message) *message = ui_locale::text(ui_locale::TextId::ImageDimensionsFailed);
        return ReaderBackgroundResult::InvalidImage;
    }

    int image_long = std::max(width, height);
    int image_short = std::min(width, height);
    int screen_long = std::max(screen_width, screen_height);
    int screen_short = std::min(screen_width, screen_height);
    if (require_full_resolution && (image_long < screen_long || image_short < screen_short)) {
        if (message) {
            char text[112];
            std::snprintf(text, sizeof(text), "%s %d x %d (%d x %d)",
                          ui_locale::text(ui_locale::TextId::ImageTooSmall),
                          screen_long, screen_short, image_long, image_short);
            *message = text;
        }
        return ReaderBackgroundResult::TooSmall;
    }
    if (!generate_cache(selected, ext, width, height, screen_width, screen_height)) {
        if (message) *message = ui_locale::text(ui_locale::TextId::BackgroundCacheFailed);
        return ReaderBackgroundResult::CacheFailed;
    }
    if (!save_path(selected)) {
        release_cache();
        unlink(CACHE_PATH);
        if (message) *message = ui_locale::text(ui_locale::TextId::BackgroundSaveFailed);
        return ReaderBackgroundResult::SaveFailed;
    }
    background_path = selected;
    background_ext = ext;
    background_width = width;
    background_height = height;
    background_progressive = progressive;
    if (message) *message = ui_locale::text(ui_locale::TextId::BackgroundSet);
    return ReaderBackgroundResult::Ok;
}

bool reader_background_clear()
{
    ensure_initialized();
    if (!save_path({})) return false;
    background_path.clear();
    background_ext.clear();
    background_width = 0;
    background_height = 0;
    background_progressive = false;
    release_cache();
    unlink(CACHE_PATH);
    unlink(CACHE_TEMP_PATH);
    return true;
}

bool reader_background_matches(const char *path)
{
    ensure_initialized();
    return path && background_path == path;
}

void reader_background_file_renamed(const char *old_path, const char *new_path)
{
    if (!reader_background_matches(old_path) || !new_path) return;
    std::string renamed = new_path;
    std::string ext = image_extension(renamed);
    if (ext.empty() || !save_path(renamed)) return;
    background_path = renamed;
    background_ext = ext;
    update_cache_identity(renamed);
}

void reader_background_file_deleted(const char *path)
{
    if (reader_background_matches(path)) reader_background_clear();
}

bool reader_background_draw(lgfx::LGFXBase &gfx, int width, int height)
{
    ensure_initialized();
    if (background_path.empty() || width <= 0 || height <= 0 ||
        background_width <= 0 || background_height <= 0 ||
        !ensure_cache(width, height)) {
        gfx.fillScreen(TFT_WHITE);
        return false;
    }

    int64_t started = esp_timer_get_time();
    lgfx::pixelcopy_t copy(background_cache, gfx.getColorDepth(),
                           lgfx::color_depth_t::grayscale_4bit,
                           gfx.hasPalette(), GRAYSCALE_PALETTE);
    gfx.pushImage(0, 0, width, height, &copy, false);
    ESP_LOGI(TAG, "cache draw: %dx%d bytes=%u time=%lld ms",
             width, height, (unsigned)background_cache_size,
             (esp_timer_get_time() - started) / 1000);
    return true;
}

Settings(bool rss = false, bool standby = false, bool book_cover = false) {
    if (standby) {
        NVS_NAMESPACE = "standby_bg";
        CACHE_PATH = "/sdcard/.reader/standby_wallpaper.g4";
        CACHE_TEMP_PATH = "/sdcard/.reader/standby_wallpaper.g4.tmp";
        require_full_resolution = false;
    }
    if (rss) {
        NVS_NAMESPACE = "rss_reader_bg";
        CACHE_PATH = "/sdcard/.reader/rss_background.g4";
        CACHE_TEMP_PATH = "/sdcard/.reader/rss_background.g4.tmp";
    }
    if (book_cover) {
        // The selected path comes from reader_book_cover; never read/write a
        // background or system-wallpaper preference for this temporary renderer.
        initialized = true;
        require_full_resolution = false;
        CACHE_PATH = "/sdcard/.reader/reader_book_cover.g4";
        CACHE_TEMP_PATH = "/sdcard/.reader/reader_book_cover.g4.tmp";
    }
}
};
Settings shared_settings;
Settings rss_settings(true);
Settings standby_settings(false, true);
Settings book_cover_settings(false, false, true);
bool rss_seeded = false;
void seed_rss_settings()
{
    if (rss_seeded) return;
    rss_settings.ensure_initialized();
    nvs_handle_t h;
    if (nvs_open(rss_settings.NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t seeded = 0;
    if (nvs_get_u8(h, "profile_v1", &seeded) != ESP_OK || !seeded) {
        shared_settings.ensure_initialized();
        rss_settings.background_path = shared_settings.background_path;
        rss_settings.background_ext = shared_settings.background_ext;
        rss_settings.background_width = shared_settings.background_width;
        rss_settings.background_height = shared_settings.background_height;
        rss_settings.background_progressive = shared_settings.background_progressive;
        if (!rss_settings.save_path(rss_settings.background_path)) { nvs_close(h); return; }
        esp_err_t err = nvs_set_u8(h, "profile_v1", 1);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err != ESP_OK) { nvs_close(h); return; }
    }
    nvs_close(h);
    rss_seeded = true;
}
Settings &settings(ReaderProfile profile) {
    if (profile == ReaderProfile::Rss) seed_rss_settings();
    return profile == ReaderProfile::Rss ? rss_settings : shared_settings;
}
} // namespace

bool reader_background_enabled(ReaderProfile profile) { return settings(profile).reader_background_enabled(); }
const char *reader_background_path(ReaderProfile profile) { return settings(profile).reader_background_path(); }
bool reader_background_clear(ReaderProfile profile) { return settings(profile).reader_background_clear(); }
bool reader_background_matches(const char *path) { return shared_settings.reader_background_matches(path) || rss_settings.reader_background_matches(path); }
void reader_background_file_renamed(const char *old_path, const char *new_path) {
    shared_settings.reader_background_file_renamed(old_path, new_path);
    rss_settings.reader_background_file_renamed(old_path, new_path);
    standby_settings.reader_background_file_renamed(old_path, new_path);
}
void reader_background_file_deleted(const char *path) {
    shared_settings.reader_background_file_deleted(path);
    rss_settings.reader_background_file_deleted(path);
    standby_settings.reader_background_file_deleted(path);
}
ReaderBackgroundResult reader_background_set(const char *path, int width, int height, std::string *message, ReaderProfile profile) {
    return settings(profile).reader_background_set(path, width, height, message);
}
bool reader_background_draw(lgfx::LGFXBase &gfx, int width, int height, ReaderProfile profile) {
    return settings(profile).reader_background_draw(gfx, width, height);
}

bool standby_wallpaper_enabled() { return standby_settings.reader_background_enabled(); }
const char* standby_wallpaper_path() { return standby_settings.reader_background_path(); }
bool standby_wallpaper_clear() { return standby_settings.reader_background_clear(); }
ReaderBackgroundResult standby_wallpaper_set(const char* path, int width, int height, std::string* message) {
    auto result = standby_settings.reader_background_set(path, width, height, message);
    standby_settings.release_cache();
    if (result == ReaderBackgroundResult::Ok && message)
        *message = ui_locale::text(ui_locale::TextId::StandbyWallpaperSet);
    return result;
}
bool standby_wallpaper_prepare(int width, int height) {
    standby_settings.ensure_initialized();
    return !standby_settings.background_path.empty() && standby_settings.ensure_cache(width, height);
}
bool standby_wallpaper_draw(lgfx::LGFXBase& gfx, int width, int height) {
    return standby_wallpaper_prepare(width, height)
        && standby_settings.reader_background_draw(gfx, width, height);
}
void standby_wallpaper_release() { standby_settings.release_cache(); }

bool reader_book_cover_prepare(const char *path, int width, int height, lgfx::LGFXBase *reading_page) {
    auto &image = book_cover_settings;
    image.release_cache();
    image.background_path = path ? path : "";
    image.background_ext = image.image_extension(image.background_path);
    image.background_width = image.background_height = 0;
    if (width <= 0 || height <= 0 || image.background_ext.empty() ||
        !image.probe_image(image.background_path, image.background_ext,
            &image.background_width, &image.background_height, &image.background_progressive)) {
        reader_book_cover_release();
        return false;
    }
    const bool prepared = image.background_ext == ".png"
        ? reading_page && image.generate_cache(image.background_path, image.background_ext,
            image.background_width, image.background_height, width, height, reading_page)
        : image.ensure_cache(width, height);
    if (!prepared) {
        reader_book_cover_release();
        return false;
    }
    return true;
}

bool reader_book_cover_draw(lgfx::LGFXBase &gfx, int width, int height) {
    return book_cover_settings.background_cache &&
        book_cover_settings.reader_background_draw(gfx, width, height);
}

void reader_book_cover_release() {
    book_cover_settings.release_cache();
    book_cover_settings.background_path.clear();
    book_cover_settings.background_ext.clear();
    book_cover_settings.background_width = book_cover_settings.background_height = 0;
}
