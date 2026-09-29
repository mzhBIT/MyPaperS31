#include "renderer/TextRenderer.h"
#include "renderer/TextLayout.h"
#include "storage/storage_sdcard.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <utility>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <esp_dma_utils.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_OUTLINE_H
#include FT_SYSTEM_H
#include FT_MODULE_H

static const char *TAG = "TEXT_RENDER";
static const int GLYPH_BITMAP_BPP = 4;
static const int BIN_GLYPH_POOL_DEFAULT_BYTES = 1024 * 1024;
static const unsigned long BIN_FONT_HEADER_SIZE = 5;
static const unsigned long BIN_FONT_ENTRY_SIZE = 20;
static const int GLYPH_CACHE_FIFO_RETAIN_PERCENT = 50;
static const int GLYPH_CACHE_FIFO_RETAIN_COUNT_PERCENT = 75;
static const bool USE_GLYPH_PUSH_GRAYIMAGE = false;
static const bool LOG_SLOW_GLYPH_PERF = false;

extern const uint8_t reader_bin_start[] asm("_binary_reader_bin_start");
extern const uint8_t reader_bin_end[] asm("_binary_reader_bin_end");

static const int FONT_STREAM_WINDOW_COUNT = 64;
static const int FONT_STREAM_MIN_WINDOWS = 4;
static const size_t FONT_STREAM_WINDOW_BYTES = 16 * 1024;

struct FontCacheWindow {
    uint8_t *data = nullptr;
    unsigned long start = 0;
    unsigned long size = 0;
    uint32_t last_used = 0;
};

struct FontStreamContext {
    std::string path;
    int fd = -1;
    FontCacheWindow windows[FONT_STREAM_WINDOW_COUNT];
    size_t window_capacity = FONT_STREAM_WINDOW_BYTES;
    int allocated_windows = 0;
    bool windows_dma_compatible = false;
    bool windows_in_psram = false;
    size_t window_dma_alignment = 0;
    unsigned long file_size = 0;
    uint32_t tick = 1;
    uint32_t stream_calls = 0;
    uint32_t cache_hits = 0;
    uint32_t resident_hits = 0;
    uint32_t cache_misses = 0;
    uint32_t physical_reads = 0;
    uint32_t direct_reads = 0;
    uint32_t fill_reads = 0;
    unsigned long cache_hit_bytes = 0;
    unsigned long physical_read_bytes = 0;
    int64_t physical_read_us = 0;
};

static uint8_t *allocate_font_stream_window(size_t size,
                                            bool *dma_compatible,
                                            bool *in_psram,
                                            size_t *dma_alignment)
{
    if (dma_compatible) *dma_compatible = false;
    if (in_psram) *in_psram = false;
    if (dma_alignment) *dma_alignment = 0;

    sdmmc_card_t *card = storage_sdcard_get_card();
    if (!card || !card->host.get_dma_info) return nullptr;

    esp_dma_mem_info_t dma_info = {};
    if (card->host.get_dma_info(card->host.slot, &dma_info) != ESP_OK) return nullptr;
    if (dma_alignment) *dma_alignment = dma_info.dma_alignment_bytes;

    void *buffer = nullptr;
    size_t actual_size = 0;
#if SOC_SDMMC_PSRAM_DMA_CAPABLE
    esp_dma_mem_info_t psram_dma_info = dma_info;
    psram_dma_info.extra_heap_caps |= MALLOC_CAP_SPIRAM;
    if (esp_dma_capable_malloc(size, &psram_dma_info, &buffer, &actual_size) == ESP_OK) {
        if (esp_dma_is_buffer_alignment_satisfied(buffer, size, psram_dma_info)) {
            if (dma_compatible) *dma_compatible = true;
            if (in_psram) *in_psram = true;
            return static_cast<uint8_t *>(buffer);
        }
        free(buffer);
        buffer = nullptr;
    }
#endif

    dma_info.extra_heap_caps |= MALLOC_CAP_INTERNAL;
    if (esp_dma_capable_malloc(size, &dma_info, &buffer, &actual_size) != ESP_OK) return nullptr;
    if (!esp_dma_is_buffer_alignment_satisfied(buffer, size, dma_info)) {
        free(buffer);
        return nullptr;
    }
    if (dma_compatible) *dma_compatible = true;
    return static_cast<uint8_t *>(buffer);
}

static void *ft_caps_alloc(FT_Memory /*memory*/, long size)
{
    if (size <= 0) return nullptr;
    void *ptr = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ptr;
}

static void ft_caps_free(FT_Memory /*memory*/, void *block)
{
    if (block) heap_caps_free(block);
}

static void *ft_caps_realloc(FT_Memory /*memory*/, long cur_size, long new_size, void *block)
{
    if (new_size <= 0) {
        if (block) heap_caps_free(block);
        return nullptr;
    }
    if (!block) return ft_caps_alloc(nullptr, new_size);

    void *ptr = ft_caps_alloc(nullptr, new_size);
    if (!ptr) return nullptr;
    if (cur_size > 0) {
        std::memcpy(ptr, block, (size_t)std::min(cur_size, new_size));
    }
    heap_caps_free(block);
    return ptr;
}

static FT_Memory create_ft_memory()
{
    FT_Memory memory = (FT_Memory)heap_caps_calloc(1, sizeof(FT_MemoryRec_), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) return nullptr;
    memory->user = nullptr;
    memory->alloc = ft_caps_alloc;
    memory->free = ft_caps_free;
    memory->realloc = ft_caps_realloc;
    return memory;
}

static size_t next_power_of_two(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static size_t hash_codepoint(uint32_t codepoint)
{
    return (size_t)(codepoint * 2654435761u);
}

static int estimate_width_no_font(uint32_t cp, int font_size)
{
    if (cp == '\t') return font_size * 2;
    if (cp < 0x80) {
        if (cp == ' ') return font_size / 3;
        if (cp == 'i' || cp == 'l' || cp == 'I' || cp == '!' || cp == '|') return font_size / 3;
        if (cp == 'm' || cp == 'w' || cp == 'M' || cp == 'W') return font_size * 3 / 4;
        return font_size / 2;
    }
    return font_size;
}

static bool is_cjk_or_fullwidth(uint32_t cp)
{
    return text_layout::is_cjk_or_fullwidth(cp);
}

static bool is_cjk_punctuation(uint32_t cp)
{
    return text_layout::is_cjk_punctuation(cp);
}

static uint16_t gray565(uint8_t v)
{
    return (uint16_t)(((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3));
}

TextRenderer &reader_text_renderer()
{
    static TextRenderer renderer;
    return renderer;
}

static unsigned long read_font_file_range(FontStreamContext *ctx, unsigned long offset, uint8_t *buffer, unsigned long count)
{
    if (!ctx || ctx->fd < 0 || !buffer || count == 0) return 0;

    int64_t t0 = esp_timer_get_time();
    if (lseek(ctx->fd, (off_t)offset, SEEK_SET) < 0) {
        return 0;
    }

    ssize_t got = read(ctx->fd, buffer, count);
    int64_t t1 = esp_timer_get_time();
    if (got > 0) {
        ctx->physical_reads++;
        ctx->physical_read_bytes += (unsigned long)got;
        ctx->physical_read_us += t1 - t0;
        return (unsigned long)got;
    }
    return 0;
}

static unsigned font_stream_cache_bytes(const FontStreamContext *ctx)
{
    if (!ctx) return 0;
    unsigned total = 0;
    for (int i = 0; i < FONT_STREAM_WINDOW_COUNT; ++i) {
        if (ctx->windows[i].data) total += (unsigned)ctx->window_capacity;
    }
    return total;
}

static void reset_font_stream_perf(FontStreamContext *ctx)
{
    if (!ctx) return;
    ctx->stream_calls = 0;
    ctx->cache_hits = 0;
    ctx->resident_hits = 0;
    ctx->cache_misses = 0;
    ctx->physical_reads = 0;
    ctx->direct_reads = 0;
    ctx->fill_reads = 0;
    ctx->cache_hit_bytes = 0;
    ctx->physical_read_bytes = 0;
    ctx->physical_read_us = 0;
}

static FontCacheWindow *pick_font_cache_window(FontStreamContext *ctx)
{
    FontCacheWindow *oldest = &ctx->windows[0];
    for (int i = 0; i < FONT_STREAM_WINDOW_COUNT; ++i) {
        FontCacheWindow *win = &ctx->windows[i];
        if (!win->data) continue;
        if (win->size == 0) return win;
        if (!oldest->data || win->last_used < oldest->last_used) oldest = win;
    }
    return oldest && oldest->data ? oldest : nullptr;
}

static unsigned long font_stream_io(FT_Stream stream, unsigned long offset, unsigned char *buffer, unsigned long count)
{
    FontStreamContext *ctx = stream ? (FontStreamContext *)stream->descriptor.pointer : nullptr;
    if (!ctx) return 0;
    ctx->stream_calls++;
    if (count == 0) return 0;
    if (!buffer) return 0;
    if (offset >= ctx->file_size) return 0;
    if (offset + count > ctx->file_size) count = ctx->file_size - offset;

    for (int i = 0; i < FONT_STREAM_WINDOW_COUNT; ++i) {
        FontCacheWindow *win = &ctx->windows[i];
        if (!win->data || win->size == 0) continue;
        if (offset >= win->start && offset + count <= win->start + win->size) {
            win->last_used = ctx->tick++;
            ctx->cache_hits++;
            ctx->resident_hits++;
            ctx->cache_hit_bytes += count;
            std::memcpy(buffer, win->data + (offset - win->start), count);
            return count;
        }
    }

    ctx->cache_misses++;

    if (count > ctx->window_capacity) {
        ctx->direct_reads++;
        return read_font_file_range(ctx, offset, buffer, count);
    }

    FontCacheWindow *win = pick_font_cache_window(ctx);
    if (!win || !win->data) {
        ctx->direct_reads++;
        return read_font_file_range(ctx, offset, buffer, count);
    }

    const unsigned long align = 4096;
    const unsigned long min_fill = FONT_STREAM_WINDOW_BYTES;
    unsigned long start = offset & ~(align - 1);
    unsigned long to_read = std::max(count, min_fill);
    if (to_read > ctx->window_capacity) to_read = (unsigned long)ctx->window_capacity;
    if (start + to_read > ctx->file_size) to_read = ctx->file_size - start;

    ctx->fill_reads++;
    unsigned long got = read_font_file_range(ctx, start, win->data, to_read);
    win->start = start;
    win->size = got;
    win->last_used = ctx->tick++;

    if (got == 0 || offset < win->start || offset + count > win->start + win->size) {
        ctx->direct_reads++;
        return read_font_file_range(ctx, offset, buffer, count);
    }

    ctx->cache_hits++;
    ctx->cache_hit_bytes += count;
    std::memcpy(buffer, win->data + (offset - win->start), count);
    return count;
}

static void font_stream_close(FT_Stream /*stream*/)
{
}

static bool create_font_stream(const char *path, void **out_stream, void **out_ctx)
{
    *out_stream = nullptr;
    *out_ctx = nullptr;
    if (!path || !path[0]) return false;

    struct stat st = {};
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 32 * 1024 * 1024) {
        ESP_LOGE(TAG, "bad font size: %ld path=%s", (long)st.st_size, path);
        return false;
    }

    FT_Stream stream = (FT_Stream)heap_caps_calloc(1, sizeof(FT_StreamRec), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FontStreamContext *ctx = new FontStreamContext();
    if (!stream || !ctx) {
        if (stream) heap_caps_free(stream);
        if (ctx) delete ctx;
        return false;
    }

    ctx->path = path;
    ctx->file_size = (unsigned long)st.st_size;
    ctx->fd = open(path, O_RDONLY);
    if (ctx->fd < 0) {
        heap_caps_free(stream);
        delete ctx;
        ESP_LOGE(TAG, "font fd open failed: %s", path);
        return false;
    }
    for (int i = 0; i < FONT_STREAM_WINDOW_COUNT; ++i) {
        bool dma_compatible = false;
        bool in_psram = false;
        size_t dma_alignment = 0;
        ctx->windows[i].data = allocate_font_stream_window(ctx->window_capacity,
                                                           &dma_compatible,
                                                           &in_psram,
                                                           &dma_alignment);
        if (!ctx->windows[i].data) {
            if (ctx->allocated_windows >= FONT_STREAM_MIN_WINDOWS) {
                ESP_LOGW(TAG, "font stream cache reduced: windows=%d/%d bytes=%u",
                         ctx->allocated_windows,
                         FONT_STREAM_WINDOW_COUNT,
                         (unsigned)(ctx->allocated_windows * ctx->window_capacity));
                break;
            }
            for (int j = 0; j < FONT_STREAM_WINDOW_COUNT; ++j) {
                if (ctx->windows[j].data) heap_caps_free(ctx->windows[j].data);
            }
            close(ctx->fd);
            heap_caps_free(stream);
            delete ctx;
            return false;
        }
        if (ctx->allocated_windows == 0) {
            ctx->windows_dma_compatible = dma_compatible;
            ctx->windows_in_psram = in_psram;
        } else {
            ctx->windows_dma_compatible = ctx->windows_dma_compatible && dma_compatible;
            ctx->windows_in_psram = ctx->windows_in_psram && in_psram;
        }
        ctx->allocated_windows++;
        ctx->window_dma_alignment = std::max(ctx->window_dma_alignment, dma_alignment);
    }

    stream->size = (unsigned long)st.st_size;
    stream->pos = 0;
    stream->descriptor.pointer = ctx;
    stream->pathname.pointer = nullptr;
    stream->read = font_stream_io;
    stream->close = font_stream_close;

    *out_stream = stream;
    *out_ctx = ctx;
    return true;
}

static void destroy_font_stream(void *stream_ptr, void *ctx_ptr)
{
    if (ctx_ptr) {
        FontStreamContext *ctx = (FontStreamContext *)ctx_ptr;
        if (ctx->fd >= 0) close(ctx->fd);
        for (int i = 0; i < FONT_STREAM_WINDOW_COUNT; ++i) {
            if (ctx->windows[i].data) heap_caps_free(ctx->windows[i].data);
        }
        delete ctx;
    }
    if (stream_ptr) heap_caps_free(stream_ptr);
}

static bool has_ext_ci(const std::string &path, const char *ext)
{
    size_t ext_len = std::strlen(ext);
    if (path.size() < ext_len) return false;
    size_t start = path.size() - ext_len;
    for (size_t i = 0; i < ext_len; ++i) {
        if (std::tolower((unsigned char)path[start + i]) != std::tolower((unsigned char)ext[i])) return false;
    }
    return true;
}

static bool is_builtin_reader_bin_path(const char *path)
{
    return path && std::strcmp(path, "builtin:reader.bin") == 0;
}

static bool is_builtin_source_han_lvfont_path(const char *path)
{
    return path && std::strcmp(path, "builtin:source_han_sans_cn_36") == 0;
}

static bool read_exact_at(int fd, unsigned long offset, void *buffer, size_t size)
{
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) return false;
    uint8_t *dst = (uint8_t *)buffer;
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, dst + got, size - got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

bool TextRenderer::begin(const ReaderTextStyle &style)
{
    std::string requested_font_path = style.font_path ? style.font_path : "";
    bool changed = _active_font_path != requested_font_path ||
                   _style.font_size != style.font_size ||
                   _style.line_spacing != style.line_spacing ||
                   _style.letter_spacing != style.letter_spacing ||
                   _style.glyph_pool_bytes != style.glyph_pool_bytes;
    if (!changed && (_ft_ready || _bin_ready || _lv_ready)) return true;

    resetFont();
    _perf = new PerfCounters();
    if (!_perf) {
        ESP_LOGE(TAG, "perf counter alloc failed");
        return false;
    }
    _style = style;
    _active_font_path = requested_font_path;
    _style.font_path = _active_font_path.c_str();

    if (is_builtin_source_han_lvfont_path(_active_font_path.c_str())) {
        return beginLvFont();
    }

    if (has_ext_ci(_active_font_path, ".bin")) {
        return beginBinFont();
    }

    _ft_memory = create_ft_memory();
    if (!_ft_memory) {
        ESP_LOGE(TAG, "FreeType memory allocator init failed");
        return false;
    }

    FT_Library lib = nullptr;
    FT_Error err = FT_New_Library((FT_Memory)_ft_memory, &lib);
    _ft_initialized = (err == 0);
    if (err != 0) {
        resetFont();
        return false;
    }
    FT_Add_Default_Modules(lib);

    _library = lib;

    FT_Face face = nullptr;
    if (!_style.font_path || !_style.font_path[0]) {
        resetFont();
        return false;
    }

    if (!create_font_stream(_style.font_path, &_font_stream, &_font_stream_ctx)) {
        resetFont();
        return false;
    }
    FontStreamContext *font_ctx = (FontStreamContext *)_font_stream_ctx;
    reset_font_stream_perf(font_ctx);

    FT_Open_Args args;
    std::memset(&args, 0, sizeof(args));
    args.flags = FT_OPEN_STREAM;
    args.stream = (FT_Stream)_font_stream;

    err = FT_Open_Face(lib, &args, 0, &face);
    if (err != 0) {
        ESP_LOGE(TAG, "FT_Open_Face(stream) failed err=%d path=%s",
                 err,
                 _style.font_path);
        resetFont();
        return false;
    }

    _face = face;
    err = FT_Set_Pixel_Sizes(face, 0, _style.font_size);
    if (err != 0) {
        resetFont();
        return false;
    }

    _baseline = (int)(face->size->metrics.ascender >> 6);
    int height = (int)(face->size->metrics.height >> 6);
    _line_height = std::max(_style.font_size, height) + _style.line_spacing;
    _advance_cache.reserve(std::max(64, _style.advance_cache_limit));
    _glyph_cache.reserve(std::max(32, _style.glyph_cache_limit));
    _glyph_pool_capacity = (size_t)std::max(4096, _style.glyph_pool_bytes);
    _glyph_pool = (uint8_t *)heap_caps_malloc(_glyph_pool_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_glyph_pool) {
        ESP_LOGW(TAG, "PSRAM glyph pool alloc failed, trying internal RAM: %u bytes", (unsigned)_glyph_pool_capacity);
        _glyph_pool = (uint8_t *)heap_caps_malloc(_glyph_pool_capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!_glyph_pool) {
        ESP_LOGE(TAG, "glyph pool alloc failed: %u bytes", (unsigned)_glyph_pool_capacity);
        resetFont();
        return false;
    }
    _glyph_pool_used = 0;
    initLookupTables();
    _ft_ready = true;
    ESP_LOGI(TAG, "begin font=%s font_bytes=%u stream_cache=%u windows=%d dma_align=%u dma_ok=%d memory=%s size=%d line=%d glyph_bpp=%d glyph_pool=%u internal_free=%u largest=%u spiram_free=%u",
             _style.font_path ? _style.font_path : "",
             font_ctx ? (unsigned)font_ctx->file_size : 0,
             font_stream_cache_bytes(font_ctx),
             font_ctx ? font_ctx->allocated_windows : 0,
             font_ctx ? (unsigned)font_ctx->window_dma_alignment : 0,
             font_ctx && font_ctx->windows_dma_compatible ? 1 : 0,
             font_ctx && font_ctx->windows_in_psram ? "PSRAM" : "internal",
             _style.font_size,
             _line_height,
             GLYPH_BITMAP_BPP,
             (unsigned)_glyph_pool_capacity,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "perf begin font_open stream_calls=%u cache_hits=%u resident_hits=%u misses=%u hit_bytes=%u physical_reads=%u read_bytes=%u read_time=%lld us direct=%u fills=%u",
             font_ctx ? (unsigned)font_ctx->stream_calls : 0,
             font_ctx ? (unsigned)font_ctx->cache_hits : 0,
             font_ctx ? (unsigned)font_ctx->resident_hits : 0,
             font_ctx ? (unsigned)font_ctx->cache_misses : 0,
             font_ctx ? (unsigned)font_ctx->cache_hit_bytes : 0,
             font_ctx ? (unsigned)font_ctx->physical_reads : 0,
             font_ctx ? (unsigned)font_ctx->physical_read_bytes : 0,
             font_ctx ? font_ctx->physical_read_us : 0,
             font_ctx ? (unsigned)font_ctx->direct_reads : 0,
             font_ctx ? (unsigned)font_ctx->fill_reads : 0);
    return true;
}

bool TextRenderer::beginBinFont()
{
    if (!_style.font_path || !_style.font_path[0]) {
        resetFont();
        return false;
    }

    int fd = -1;
    size_t data_size = 0;
    if (is_builtin_reader_bin_path(_style.font_path)) {
        _bin_data = reader_bin_start;
        _bin_data_size = (size_t)(reader_bin_end - reader_bin_start);
        data_size = _bin_data_size;
    } else {
        fd = open(_style.font_path, O_RDONLY);
        if (fd < 0) {
            ESP_LOGE(TAG, "bin font open failed path=%s", _style.font_path);
            resetFont();
            return false;
        }

        struct stat st = {};
        if (fstat(fd, &st) != 0 || st.st_size < 5) {
            ESP_LOGE(TAG, "bin font stat failed path=%s", _style.font_path);
            close(fd);
            resetFont();
            return false;
        }
        data_size = (size_t)st.st_size;
    }

    uint8_t header[5] = {};
    _bin_fd = fd;
    if (!readBinDataAt(0, header, sizeof(header))) {
        ESP_LOGE(TAG, "bin font header read failed path=%s", _style.font_path);
        resetFont();
        return false;
    }

    uint32_t char_count = (uint32_t)header[0] |
                          ((uint32_t)header[1] << 8) |
                          ((uint32_t)header[2] << 16) |
                          ((uint32_t)header[3] << 24);
    uint8_t font_height = header[4];
    unsigned long entries_bytes = (unsigned long)char_count * BIN_FONT_ENTRY_SIZE;
    unsigned long bitmap_base = BIN_FONT_HEADER_SIZE + entries_bytes;
    if (char_count == 0 || char_count > 65536 || font_height == 0 ||
        bitmap_base > (unsigned long)data_size) {
        ESP_LOGE(TAG, "bad bin font header count=%lu height=%u size=%lu path=%s",
                 (unsigned long)char_count,
                 (unsigned)font_height,
                 (unsigned long)data_size,
                 _style.font_path);
        resetFont();
        return false;
    }

    _bin_char_count = char_count;
    _bin_font_height = font_height;
    _bin_file_size = (unsigned long)data_size;
    _bin_entry_table_bytes = (size_t)entries_bytes;
    _bin_entry_table = (uint8_t *)heap_caps_malloc(_bin_entry_table_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_bin_entry_table) {
        ESP_LOGW(TAG, "PSRAM bin entry table alloc failed, trying internal RAM: %u bytes", (unsigned)_bin_entry_table_bytes);
        _bin_entry_table = (uint8_t *)heap_caps_malloc(_bin_entry_table_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (_bin_entry_table) {
        int64_t entry_t0 = esp_timer_get_time();
        if (readBinDataAt(BIN_FONT_HEADER_SIZE, _bin_entry_table, _bin_entry_table_bytes)) {
            ESP_LOGI(TAG, "bin entry table cached bytes=%u time=%lld ms",
                     (unsigned)_bin_entry_table_bytes,
                     (esp_timer_get_time() - entry_t0) / 1000);
        } else {
            ESP_LOGW(TAG, "bin entry table cache read failed, using on-demand reads");
            heap_caps_free(_bin_entry_table);
            _bin_entry_table = nullptr;
            _bin_entry_table_bytes = 0;
        }
    } else {
        ESP_LOGW(TAG, "bin entry table cache alloc failed, using on-demand reads: %u bytes", (unsigned)_bin_entry_table_bytes);
        _bin_entry_table_bytes = 0;
    }
    detectBinFontEncoding();
    _baseline = std::max(1, (int)font_height * 4 / 5);
    _line_height = (int)font_height + _style.line_spacing;
    _advance_cache.reserve(std::max(64, _style.advance_cache_limit));
    _bin_entry_cache.reserve(std::max(64, _style.advance_cache_limit));
    _glyph_cache.reserve(std::max(32, _style.glyph_cache_limit));
    _glyph_pool_capacity = (size_t)std::max(BIN_GLYPH_POOL_DEFAULT_BYTES, _style.glyph_pool_bytes);
    _glyph_pool = (uint8_t *)heap_caps_malloc(_glyph_pool_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_glyph_pool) {
        ESP_LOGW(TAG, "PSRAM bin glyph pool alloc failed, trying internal RAM: %u bytes", (unsigned)_glyph_pool_capacity);
        _glyph_pool = (uint8_t *)heap_caps_malloc(_glyph_pool_capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!_glyph_pool) {
        ESP_LOGE(TAG, "bin glyph pool alloc failed: %u bytes", (unsigned)_glyph_pool_capacity);
        resetFont();
        return false;
    }
    _glyph_pool_used = 0;
    initLookupTables();
    _bin_ready = true;
    ESP_LOGI(TAG, "begin bin font=%s font_bytes=%u chars=%lu size=%u line=%d glyph_bpp=%d glyph_pool=%u entry_cache=%u internal_free=%u largest=%u spiram_free=%u",
             _style.font_path ? _style.font_path : "",
             (unsigned)_bin_file_size,
             (unsigned long)_bin_char_count,
             (unsigned)_bin_font_height,
             _line_height,
             GLYPH_BITMAP_BPP,
             (unsigned)_glyph_pool_capacity,
             (unsigned)_bin_entry_table_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

bool TextRenderer::beginLvFont()
{
    _lv_font = &fonts::lv_font_SourceHanSansNormalCN_36;
    lgfx::FontMetrics metrics;
    _lv_font->getDefaultMetric(&metrics);
    _baseline = std::max(1, (int)metrics.baseline);
    _line_height = std::max(1, (int)metrics.height) + _style.line_spacing;
    _lv_ready = true;
    ESP_LOGI(TAG, "begin lv font=%s line=%d baseline=%d",
             _style.font_path ? _style.font_path : "",
             _line_height,
             _baseline);
    return true;
}

void TextRenderer::resetFont()
{
    resetBinFont();
    if (_face) {
        FT_Done_Face((FT_Face)_face);
        _face = nullptr;
    }
    bool library_was_active = _library != nullptr;
    if (_library) {
        FT_Done_FreeType((FT_Library)_library);
        _library = nullptr;
    }
    if (_ft_memory && !library_was_active) {
        heap_caps_free(_ft_memory);
    }
    _ft_memory = nullptr;
    destroy_font_stream(_font_stream, _font_stream_ctx);
    _font_stream = nullptr;
    _font_stream_ctx = nullptr;
    _ft_ready = false;
    _ft_initialized = false;
    _lv_ready = false;
    _lv_font = nullptr;
    _advance_cache.clear();
    _glyph_cache.clear();
    _missing_codepoints.clear();
    _page_missing_codepoints.clear();
    _prepared_page_codepoints.clear();
    _advance_lookup.clear();
    _glyph_lookup.clear();
    if (_glyph_pool) {
        heap_caps_free(_glyph_pool);
        _glyph_pool = nullptr;
    }
    _glyph_pool_capacity = 0;
    _glyph_pool_used = 0;
    _glyph_pool_full = false;
    _lru_tick = 1;
    _line_height = 44;
    _baseline = 34;
    if (_perf) {
        delete _perf;
        _perf = nullptr;
    }
}

void TextRenderer::resetBinFont()
{
    if (_bin_fd >= 0) {
        close(_bin_fd);
        _bin_fd = -1;
    }
    _bin_ready = false;
    _bin_data = nullptr;
    _bin_data_size = 0;
    _bin_char_count = 0;
    _bin_font_height = 0;
    _bin_encoding = BIN_FONT_ENCODING_UNKNOWN;
    _bin_file_size = 0;
    if (_bin_entry_table) {
        heap_caps_free(_bin_entry_table);
        _bin_entry_table = nullptr;
    }
    _bin_entry_table_bytes = 0;
    _bin_entry_cache.clear();
}

bool TextRenderer::readBinDataAt(unsigned long offset, void *buffer, size_t size) const
{
    if (!buffer || size == 0) return true;
    if (_bin_data) {
        if (offset > _bin_data_size || size > _bin_data_size - offset) return false;
        std::memcpy(buffer, _bin_data + offset, size);
        return true;
    }
    if (_bin_fd < 0) return false;
    return read_exact_at(_bin_fd, offset, buffer, size);
}

int TextRenderer::lineHeight() const
{
    if (_ft_ready || _bin_ready || _lv_ready) return _line_height;
    return _style.font_size + _style.line_spacing;
}

int TextRenderer::baseline() const
{
    if (_ft_ready || _bin_ready || _lv_ready) return _baseline;
    return _style.font_size;
}

size_t TextRenderer::nextUtf8(const std::string &text, size_t pos, uint32_t &codepoint) const
{
    if (pos >= text.size()) {
        codepoint = 0;
        return pos;
    }

    const uint8_t c = (uint8_t)text[pos];
    if (c < 0x80) {
        codepoint = c;
        return pos + 1;
    }

    int len = 1;
    uint32_t cp = 0xFFFD;
    if ((c & 0xE0) == 0xC0 && pos + 1 < text.size()) {
        len = 2;
        cp = ((c & 0x1F) << 6) | ((uint8_t)text[pos + 1] & 0x3F);
    } else if ((c & 0xF0) == 0xE0 && pos + 2 < text.size()) {
        len = 3;
        cp = ((c & 0x0F) << 12) |
             (((uint8_t)text[pos + 1] & 0x3F) << 6) |
             ((uint8_t)text[pos + 2] & 0x3F);
    } else if ((c & 0xF8) == 0xF0 && pos + 3 < text.size()) {
        len = 4;
        cp = ((c & 0x07) << 18) |
             (((uint8_t)text[pos + 1] & 0x3F) << 12) |
             (((uint8_t)text[pos + 2] & 0x3F) << 6) |
             ((uint8_t)text[pos + 3] & 0x3F);
    }
    codepoint = cp;
    return pos + len;
}

std::string TextRenderer::codepointToUtf8(uint32_t cp) const
{
    std::string out;
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    return out;
}

void TextRenderer::initLookupTables()
{
    int advance_limit = std::max(64, _style.advance_cache_limit);
    int glyph_limit = std::max(32, _style.glyph_cache_limit);
    _advance_lookup.assign(next_power_of_two((size_t)advance_limit * 2), LookupSlot());
    _glyph_lookup.assign(next_power_of_two((size_t)glyph_limit * 2), LookupSlot());
}

int TextRenderer::findLookup(const std::vector<LookupSlot> &table, uint32_t codepoint) const
{
    if (table.empty()) return -1;

    size_t mask = table.size() - 1;
    size_t pos = hash_codepoint(codepoint) & mask;
    for (size_t probe = 0; probe < table.size(); ++probe) {
        const LookupSlot &slot = table[pos];
        if (slot.state == 0) return -1;
        if (slot.state == 1 && slot.codepoint == codepoint) return slot.index;
        pos = (pos + 1) & mask;
    }
    return -1;
}

void TextRenderer::insertLookup(std::vector<LookupSlot> &table, uint32_t codepoint, uint16_t index)
{
    if (table.empty()) return;

    size_t mask = table.size() - 1;
    size_t pos = hash_codepoint(codepoint) & mask;
    size_t first_deleted = table.size();
    for (size_t probe = 0; probe < table.size(); ++probe) {
        LookupSlot &slot = table[pos];
        if (slot.state == 1 && slot.codepoint == codepoint) {
            slot.index = index;
            return;
        }
        if (slot.state == 2 && first_deleted == table.size()) first_deleted = pos;
        if (slot.state == 0) {
            size_t target = first_deleted != table.size() ? first_deleted : pos;
            table[target].codepoint = codepoint;
            table[target].index = index;
            table[target].state = 1;
            return;
        }
        pos = (pos + 1) & mask;
    }
    if (first_deleted != table.size()) {
        table[first_deleted].codepoint = codepoint;
        table[first_deleted].index = index;
        table[first_deleted].state = 1;
    }
}

void TextRenderer::eraseLookup(std::vector<LookupSlot> &table, uint32_t codepoint)
{
    if (table.empty()) return;

    size_t mask = table.size() - 1;
    size_t pos = hash_codepoint(codepoint) & mask;
    for (size_t probe = 0; probe < table.size(); ++probe) {
        LookupSlot &slot = table[pos];
        if (slot.state == 0) return;
        if (slot.state == 1 && slot.codepoint == codepoint) {
            slot.state = 2;
            return;
        }
        pos = (pos + 1) & mask;
    }
}

bool TextRenderer::loadAdvance(uint32_t codepoint, Advance &out)
{
    if (_bin_ready) {
        BinFontEntry entry;
        if (!loadBinFontEntry(codepoint, &entry)) return false;
        out.codepoint = codepoint;
        out.advance = entry.advance;
        out.last_used = _lru_tick++;
        return true;
    }

    if (!_ft_ready || !_face) return false;

    FT_Face face = (FT_Face)_face;
    FT_Int32 load_flags = FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
    int64_t t0 = esp_timer_get_time();
    FT_UInt glyph_index = FT_Get_Char_Index(face, (FT_ULong)codepoint);
    FT_Fixed fixed_advance = 0;
    FT_Error err = FT_Get_Advance(face, glyph_index,
                                 load_flags | FT_ADVANCE_FLAG_FAST_ONLY,
                                 &fixed_advance);
    if (err != 0) {
        err = FT_Load_Glyph(face, glyph_index, load_flags);
        if (err == 0) fixed_advance = face->glyph->advance.x << 10;
    }
    int64_t t1 = esp_timer_get_time();
    if (_perf) {
        _perf->advance_loads++;
        _perf->advance_load_us += t1 - t0;
    }
    if (err != 0) return false;

    out.codepoint = codepoint;
    out.advance = (int)((fixed_advance + 0x8000) >> 16);
    out.last_used = _lru_tick++;
    return true;
}

const TextRenderer::Advance *TextRenderer::getAdvance(uint32_t codepoint)
{
    int found = findLookup(_advance_lookup, codepoint);
    if (found >= 0 && found < (int)_advance_cache.size()) {
        Advance &adv = _advance_cache[(size_t)found];
        adv.last_used = _lru_tick++;
        return &adv;
    }

    Advance adv;
    if (!loadAdvance(codepoint, adv)) return nullptr;

    int limit = std::max(64, _style.advance_cache_limit);
    if ((int)_advance_cache.size() >= limit) {
        auto victim = std::min_element(_advance_cache.begin(), _advance_cache.end(),
                                       [](const Advance &a, const Advance &b) {
                                           return a.last_used < b.last_used;
                                       });
        if (victim != _advance_cache.end()) {
            eraseLookup(_advance_lookup, victim->codepoint);
            *victim = adv;
            insertLookup(_advance_lookup, adv.codepoint, (uint16_t)(victim - _advance_cache.begin()));
            return &*victim;
        }
    } else {
        _advance_cache.push_back(adv);
        insertLookup(_advance_lookup, adv.codepoint, (uint16_t)(_advance_cache.size() - 1));
        return &_advance_cache.back();
    }
    return nullptr;
}

bool TextRenderer::loadBinFontEntry(uint32_t codepoint, BinFontEntry *out) const
{
    if (!_bin_ready || !out || codepoint > 0xFFFF) return false;
    int64_t t0 = esp_timer_get_time();
    auto cached = std::lower_bound(_bin_entry_cache.begin(), _bin_entry_cache.end(), codepoint,
                                   [](const BinFontEntry &entry, uint32_t cp) {
                                       return entry.codepoint < cp;
                                   });
    if (cached != _bin_entry_cache.end() && cached->codepoint == codepoint) {
        *out = *cached;
        if (_perf) {
            const_cast<TextRenderer *>(this)->_perf->bin_entry_us += esp_timer_get_time() - t0;
        }
        return true;
    }

    int lo = 0;
    int hi = (int)_bin_char_count - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        BinFontEntry e;
        if (!readBinFontEntryAt((uint32_t)mid, &e)) return false;
        if (_perf && !_bin_entry_table) const_cast<TextRenderer *>(this)->_perf->bin_entry_reads++;
        if (e.codepoint < codepoint) {
            lo = mid + 1;
            continue;
        }
        if (e.codepoint > codepoint) {
            hi = mid - 1;
            continue;
        }

        *out = e;
        TextRenderer *self = const_cast<TextRenderer *>(this);
        int entry_limit = std::max(64, self->_style.advance_cache_limit);
        if ((int)self->_bin_entry_cache.size() < entry_limit) {
            self->_bin_entry_cache.insert(cached, e);
        }
        if (_perf) {
            self->_perf->bin_entry_us += esp_timer_get_time() - t0;
        }
        return true;
    }
    if (_perf) {
        const_cast<TextRenderer *>(this)->_perf->bin_entry_us += esp_timer_get_time() - t0;
    }
    return false;
}

bool TextRenderer::readBinFontEntryAt(uint32_t index, BinFontEntry *out) const
{
    if (!out || (!_bin_data && _bin_fd < 0) || index >= _bin_char_count) return false;
    if (_bin_entry_table && (size_t)(index + 1) * BIN_FONT_ENTRY_SIZE <= _bin_entry_table_bytes) {
        decodeBinFontEntry(_bin_entry_table + (size_t)index * BIN_FONT_ENTRY_SIZE, out);
        return true;
    }

    uint8_t raw[20] = {};
    if (!readBinDataAt(BIN_FONT_HEADER_SIZE + (unsigned long)index * BIN_FONT_ENTRY_SIZE, raw, sizeof(raw))) {
        return false;
    }
    decodeBinFontEntry(raw, out);
    return true;
}

void TextRenderer::decodeBinFontEntry(const uint8_t *raw, BinFontEntry *out) const
{
    if (!raw || !out) return;
    BinFontEntry e;
    e.codepoint = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8);
    e.advance = (uint16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    e.width = raw[4];
    e.height = raw[5];
    e.x_offset = (int8_t)raw[6];
    e.y_offset = (int8_t)raw[7];
    e.bitmap_offset = (uint32_t)raw[8] |
                      ((uint32_t)raw[9] << 8) |
                      ((uint32_t)raw[10] << 16) |
                      ((uint32_t)raw[11] << 24);
    e.bitmap_size = (uint32_t)raw[12] |
                    ((uint32_t)raw[13] << 8) |
                    ((uint32_t)raw[14] << 16) |
                    ((uint32_t)raw[15] << 24);
    *out = e;
}

void TextRenderer::detectBinFontEncoding()
{
    _bin_encoding = BIN_FONT_ENCODING_EDC_BITSTREAM;
    uint32_t inspected = 0;
    uint32_t raw_like = 0;
    uint32_t compressed_like = 0;

    for (uint32_t i = 0; i < _bin_char_count && inspected < 64; ++i) {
        BinFontEntry e;
        if (!readBinFontEntryAt(i, &e)) break;
        if (e.width == 0 || e.height == 0 || e.bitmap_size == 0) continue;
        uint32_t raw_size = (uint32_t)(((uint32_t)e.width * (uint32_t)e.height + 1) >> 1);
        if (e.bitmap_size == raw_size) raw_like++;
        else compressed_like++;
        inspected++;
    }

    if (inspected > 0 && raw_like > 0 && compressed_like == 0) {
        _bin_encoding = BIN_FONT_ENCODING_RAW_4BPP;
    }
    ESP_LOGI(TAG, "bin font encoding=%s inspected=%lu raw=%lu compressed=%lu",
             _bin_encoding == BIN_FONT_ENCODING_RAW_4BPP ? "raw4bpp" : "edc-bitstream",
             (unsigned long)inspected,
             (unsigned long)raw_like,
             (unsigned long)compressed_like);
}

bool TextRenderer::loadBinGlyph(uint32_t codepoint, Glyph &out)
{
    int64_t total_t0 = esp_timer_get_time();
    BinFontEntry entry;
    if (!loadBinFontEntry(codepoint, &entry)) return false;
    if (_perf) _perf->bin_glyph_loads++;

    out.codepoint = codepoint;
    out.advance = entry.advance;
    out.bitmap_left = entry.x_offset;
    out.bitmap_top = _baseline - entry.y_offset;
    out.width = entry.width;
    out.height = entry.height;
    out.pitch = (entry.width + 1) >> 1;
    out.data_offset = 0;
    out.data_size = (size_t)out.pitch * (size_t)out.height;
    out.last_used = _lru_tick++;

    if (entry.bitmap_size == 0 || out.width <= 0 || out.height <= 0) {
        out.width = 0;
        out.height = 0;
        out.pitch = 0;
        out.data_size = 0;
        return true;
    }

    unsigned long bitmap_base = BIN_FONT_HEADER_SIZE + (unsigned long)_bin_char_count * BIN_FONT_ENTRY_SIZE;
    unsigned long bitmap_end = (unsigned long)entry.bitmap_offset + (unsigned long)entry.bitmap_size;
    uint32_t packed_size = (uint32_t)(((uint32_t)out.width * (uint32_t)out.height + 1) >> 1);
    if (entry.bitmap_offset < bitmap_base || bitmap_end > _bin_file_size) {
        ESP_LOGW(TAG, "bad bin glyph entry cp=U+%04lx offset=%lu size=%u w=%d h=%d file=%u",
                 (unsigned long)codepoint,
                 (unsigned long)entry.bitmap_offset,
                 (unsigned)entry.bitmap_size,
                 out.width,
                 out.height,
                 (unsigned)_bin_file_size);
        return false;
    }

    if (out.width > 255 || out.height > 255 || out.pitch * out.height > 32768) {
        ESP_LOGW(TAG, "bad bin glyph cp=U+%04lx left=%d top=%d w=%d h=%d pitch=%d",
                 (unsigned long)codepoint,
                 out.bitmap_left,
                 out.bitmap_top,
                 out.width,
                 out.height,
                 out.pitch);
        return false;
    }

    if (!_glyph_pool || _glyph_pool_used + out.data_size > _glyph_pool_capacity) {
        ESP_LOGD(TAG, "bin glyph pool full cp=U+%04lx need=%u used=%u cap=%u",
                 (unsigned long)codepoint,
                 (unsigned)out.data_size,
                 (unsigned)_glyph_pool_used,
                 (unsigned)_glyph_pool_capacity);
        _glyph_pool_full = true;
        return false;
    }

    out.data_offset = _glyph_pool_used;
    uint8_t *src_buf = (uint8_t *)heap_caps_malloc(entry.bitmap_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src_buf) {
        src_buf = (uint8_t *)heap_caps_malloc(entry.bitmap_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!src_buf) {
        ESP_LOGW(TAG, "bin glyph temp alloc failed cp=U+%04lx size=%u",
                 (unsigned long)codepoint,
                 (unsigned)entry.bitmap_size);
        out.data_offset = 0;
        out.data_size = 0;
        return false;
    }

    int64_t read_t0 = esp_timer_get_time();
    bool read_ok = readBinDataAt(entry.bitmap_offset, src_buf, entry.bitmap_size);
    int64_t read_t1 = esp_timer_get_time();
    if (_perf) {
        _perf->bin_bitmap_reads++;
        _perf->bin_bitmap_bytes += entry.bitmap_size;
        _perf->bin_bitmap_read_us += read_t1 - read_t0;
    }
    if (!read_ok) {
        ESP_LOGW(TAG, "bin glyph bitmap read failed cp=U+%04lx offset=%lu size=%u",
                 (unsigned long)codepoint,
                 (unsigned long)entry.bitmap_offset,
                 (unsigned)entry.bitmap_size);
        heap_caps_free(src_buf);
        out.data_offset = 0;
        out.data_size = 0;
        return false;
    }
    int64_t decode_t0 = esp_timer_get_time();
    uint8_t *dst = _glyph_pool + out.data_offset;
    std::memset(dst, 0, out.data_size);
    auto put_pixel = [&](uint32_t pixel_index, uint8_t src_level) {
        if (pixel_index >= (uint32_t)out.width * (uint32_t)out.height) return;
        int row = (int)(pixel_index / (uint32_t)out.width);
        int col = (int)(pixel_index % (uint32_t)out.width);
        uint8_t level = (uint8_t)(15 - (src_level & 0x0F));
        uint8_t *d = dst + (size_t)row * (size_t)out.pitch + (col >> 1);
        if ((col & 1) == 0) {
            *d = (uint8_t)((*d & 0x0F) | (level << 4));
        } else {
            *d = (uint8_t)((*d & 0xF0) | level);
        }
    };

    if (_bin_encoding == BIN_FONT_ENCODING_RAW_4BPP) {
        if (entry.bitmap_size != packed_size) {
            ESP_LOGW(TAG, "raw bin glyph size mismatch cp=U+%04lx expected=%lu actual=%u",
                     (unsigned long)codepoint,
                     (unsigned long)packed_size,
                     (unsigned)entry.bitmap_size);
            heap_caps_free(src_buf);
            out.data_offset = 0;
            out.data_size = 0;
            return false;
        }
        for (int row = 0; row < out.height; ++row) {
            for (int col = 0; col < out.width; ++col) {
                uint32_t pixel_index = (uint32_t)row * (uint32_t)out.width + (uint32_t)col;
                uint8_t packed = src_buf[pixel_index >> 1];
                uint8_t src_level = (pixel_index & 1) ? (packed & 0x0F) : (packed >> 4);
                put_pixel(pixel_index, src_level);
            }
        }
    } else {
        uint32_t pixel_index = 0;
        uint32_t total_pixels = (uint32_t)out.width * (uint32_t)out.height;
        uint32_t bit_pos = 0;
        uint32_t total_bits = entry.bitmap_size * 8;
        auto read_bit = [&]() -> int {
            if (bit_pos >= total_bits) return -1;
            uint8_t byte = src_buf[bit_pos >> 3];
            int bit = 7 - (int)(bit_pos & 7);
            bit_pos++;
            return (byte >> bit) & 1;
        };

        while (pixel_index < total_pixels && bit_pos < total_bits) {
            int b0 = read_bit();
            if (b0 < 0) break;
            if (b0 == 0) {
                put_pixel(pixel_index++, 15);
                continue;
            }

            int b1 = read_bit();
            if (b1 < 0) break;
            if (b1 == 0) {
                put_pixel(pixel_index++, 0);
                continue;
            }

            uint8_t gray = 0;
            for (int n = 0; n < 4; ++n) {
                int b = read_bit();
                gray = (uint8_t)((gray << 1) | (b > 0 ? 1 : 0));
            }
            put_pixel(pixel_index++, gray);
        }
        if (pixel_index < total_pixels) {
            ESP_LOGW(TAG, "bin glyph decode short cp=U+%04lx pixels=%lu/%lu size=%u",
                     (unsigned long)codepoint,
                     (unsigned long)pixel_index,
                     (unsigned long)total_pixels,
                     (unsigned)entry.bitmap_size);
        }
    }
    int64_t decode_t1 = esp_timer_get_time();
    if (_perf) {
        int64_t total_us = esp_timer_get_time() - total_t0;
        _perf->bin_decode_us += decode_t1 - decode_t0;
        _perf->bin_total_us += total_us;
        _perf->glyph_total_us += total_us;
        if (total_us > _perf->slowest_glyph_us) {
            _perf->slowest_glyph_us = total_us;
            _perf->slowest_glyph_cp = codepoint;
        }
    }
    heap_caps_free(src_buf);
    _glyph_pool_used += out.data_size;
    return true;
}

bool TextRenderer::loadGlyph(uint32_t codepoint, Glyph &out)
{
    if (_bin_ready) return loadBinGlyph(codepoint, out);
    if (!_ft_ready || !_face) return false;

    int64_t total_t0 = esp_timer_get_time();
    FontStreamContext *font_ctx = (FontStreamContext *)_font_stream_ctx;
    uint32_t io_reads_before = font_ctx ? font_ctx->physical_reads : 0;
    unsigned long io_bytes_before = font_ctx ? font_ctx->physical_read_bytes : 0;
    int64_t io_us_before = font_ctx ? font_ctx->physical_read_us : 0;
    uint32_t io_calls_before = font_ctx ? font_ctx->stream_calls : 0;
    uint32_t io_hits_before = font_ctx ? font_ctx->cache_hits : 0;
    FT_Face face = (FT_Face)_face;
    FT_Int32 load_flags = FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
    int64_t load_t0 = esp_timer_get_time();
    FT_Error err = FT_Load_Char(face, (FT_ULong)codepoint, load_flags);
    int64_t load_t1 = esp_timer_get_time();
    if (_perf) {
        _perf->glyph_loads++;
        _perf->glyph_load_char_us += load_t1 - load_t0;
    }
    if (err != 0) {
        ESP_LOGW(TAG, "FT_Load_Char failed cp=U+%04lx err=%d", (unsigned long)codepoint, err);
        return false;
    }

    FT_GlyphSlot slot = face->glyph;

    out.codepoint = codepoint;
    out.advance = (int)(slot->advance.x >> 6);
    out.data_offset = 0;
    out.data_size = 0;
    out.last_used = _lru_tick++;

    if (slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->outline.n_points <= 0) {
        out.bitmap_left = 0;
        out.bitmap_top = 0;
        out.width = 0;
        out.height = 0;
        out.pitch = 0;
        if (_perf) {
            int64_t total_us = esp_timer_get_time() - total_t0;
            _perf->glyph_total_us += total_us;
            if (total_us > _perf->slowest_glyph_us) {
                _perf->slowest_glyph_us = total_us;
                _perf->slowest_glyph_cp = codepoint;
            }
        }
        return true;
    }

    int64_t bbox_t0 = esp_timer_get_time();
    FT_BBox cbox;
    FT_Outline_Get_CBox(&slot->outline, &cbox);
    int x_min = (int)(cbox.xMin >> 6);
    int y_min = (int)(cbox.yMin >> 6);
    int x_max = (int)((cbox.xMax + 63) >> 6);
    int y_max = (int)((cbox.yMax + 63) >> 6);

    out.bitmap_left = x_min;
    out.bitmap_top = y_max;
    out.width = x_max - x_min;
    out.height = y_max - y_min;
    out.pitch = (out.width + 1) >> 1;
    int64_t bbox_t1 = esp_timer_get_time();
    if (_perf) _perf->glyph_bbox_us += bbox_t1 - bbox_t0;

    if (out.width < 0 || out.height < 0 || out.pitch < 0 ||
        out.width > 256 || out.height > 256 ||
        out.pitch * out.height > 32768) {
        ESP_LOGW(TAG, "bad outline bbox cp=U+%04lx left=%d top=%d w=%d h=%d pitch=%d",
                 (unsigned long)codepoint,
                 out.bitmap_left,
                 out.bitmap_top,
                 out.width,
                 out.height,
                 out.pitch);
        return false;
    }

    out.data_size = (size_t)out.pitch * (size_t)out.height;
    if (out.data_size == 0) return true;

    const int gray_pitch = out.width;
    const size_t gray_size = (size_t)gray_pitch * (size_t)out.height;
    uint8_t *gray_buf = (uint8_t *)heap_caps_malloc(gray_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gray_buf) {
        gray_buf = (uint8_t *)heap_caps_malloc(gray_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!gray_buf) {
        ESP_LOGW(TAG, "gray glyph temp alloc failed cp=U+%04lx size=%u",
                 (unsigned long)codepoint,
                 (unsigned)gray_size);
        return false;
    }

    int64_t clear_t0 = esp_timer_get_time();
    std::memset(gray_buf, 0, gray_size);
    int64_t clear_t1 = esp_timer_get_time();
    if (_perf) _perf->glyph_clear_us += clear_t1 - clear_t0;

    FT_Bitmap target;
    std::memset(&target, 0, sizeof(target));
    target.rows = (unsigned int)out.height;
    target.width = (unsigned int)out.width;
    target.pitch = gray_pitch;
    target.buffer = gray_buf;
    target.pixel_mode = FT_PIXEL_MODE_GRAY;
    target.num_grays = 256;

    FT_Outline_Translate(&slot->outline, -x_min * 64, -y_min * 64);
    int64_t raster_t0 = esp_timer_get_time();
    err = FT_Outline_Get_Bitmap((FT_Library)_library, &slot->outline, &target);
    int64_t raster_t1 = esp_timer_get_time();
    FT_Outline_Translate(&slot->outline, x_min * 64, y_min * 64);
    if (_perf) _perf->glyph_raster_us += raster_t1 - raster_t0;
    if (err != 0) {
        ESP_LOGW(TAG, "FT_Outline_Get_Bitmap failed cp=U+%04lx err=%d", (unsigned long)codepoint, err);
        heap_caps_free(gray_buf);
        out.data_offset = 0;
        out.data_size = 0;
        return false;
    }

    if (!_glyph_pool || _glyph_pool_used + out.data_size > _glyph_pool_capacity) {
        ESP_LOGW(TAG, "glyph pool full cp=U+%04lx need=%u used=%u cap=%u bpp=%d",
                 (unsigned long)codepoint,
                 (unsigned)out.data_size,
                 (unsigned)_glyph_pool_used,
                 (unsigned)_glyph_pool_capacity,
                 GLYPH_BITMAP_BPP);
        heap_caps_free(gray_buf);
        _glyph_pool_full = true;
        return false;
    }

    out.data_offset = _glyph_pool_used;
    uint8_t *dst_base = _glyph_pool + out.data_offset;
    std::memset(dst_base, 0, out.data_size);
    _glyph_pool_used += out.data_size;

    for (int row = 0; row < out.height; ++row) {
        const uint8_t *src = gray_buf + row * gray_pitch;
        uint8_t *dst = dst_base + row * out.pitch;
        for (int col = 0; col < out.width; ++col) {
            uint8_t level = (uint8_t)((src[col] + 8) / 17);
            if (level > 15) level = 15;
            if ((col & 1) == 0) {
                dst[col >> 1] = (uint8_t)(level << 4);
            } else {
                dst[col >> 1] |= level;
            }
        }
    }
    heap_caps_free(gray_buf);

    if (_perf) {
        int64_t total_us = esp_timer_get_time() - total_t0;
        _perf->glyph_total_us += total_us;
        if (total_us > _perf->slowest_glyph_us) {
            _perf->slowest_glyph_us = total_us;
            _perf->slowest_glyph_cp = codepoint;
        }
        if (LOG_SLOW_GLYPH_PERF && total_us >= 20000) {
            ESP_LOGI(TAG, "perf slow_glyph cp=U+%04lx total=%lld us load=%lld us bbox=%lld us clear=%lld us raster=%lld us io_calls=%u io_hits=%u io_reads=%u io_bytes=%u io_read=%lld us w=%d h=%d bytes=%u bpp=%d",
                     (unsigned long)codepoint,
                     total_us,
                     load_t1 - load_t0,
                     bbox_t1 - bbox_t0,
                     clear_t1 - clear_t0,
                     raster_t1 - raster_t0,
                     font_ctx ? (unsigned)(font_ctx->stream_calls - io_calls_before) : 0,
                     font_ctx ? (unsigned)(font_ctx->cache_hits - io_hits_before) : 0,
                     font_ctx ? (unsigned)(font_ctx->physical_reads - io_reads_before) : 0,
                     font_ctx ? (unsigned)(font_ctx->physical_read_bytes - io_bytes_before) : 0,
                     font_ctx ? (font_ctx->physical_read_us - io_us_before) : 0,
                     out.width,
                     out.height,
                     (unsigned)out.data_size,
                     GLYPH_BITMAP_BPP);
        }
    }
    return true;
}

const TextRenderer::Glyph *TextRenderer::getGlyph(uint32_t codepoint)
{
    if (std::find(_missing_codepoints.begin(), _missing_codepoints.end(), codepoint) != _missing_codepoints.end()) {
        return nullptr;
    }

    int found = findLookup(_glyph_lookup, codepoint);
    if (found >= 0 && found < (int)_glyph_cache.size()) {
        Glyph &glyph = _glyph_cache[(size_t)found];
        glyph.last_used = _lru_tick++;
        return &glyph;
    }

    int limit = std::max(32, _style.glyph_cache_limit);
    if ((int)_glyph_cache.size() >= limit) {
        ESP_LOGI(TAG, "glyph metadata cache full, compacting cache cp=U+%04lx count=%u limit=%d",
                 (unsigned long)codepoint,
                 (unsigned)_glyph_cache.size(),
                 limit);
        if (!compactGlyphCache(0) && (int)_glyph_cache.size() >= limit) {
            clearGlyphCache();
        }
    }

    Glyph glyph{};
    _glyph_pool_full = false;
    if (!loadGlyph(codepoint, glyph)) {
        if (_glyph_pool_full) {
            ESP_LOGI(TAG, "glyph pool exhausted, compacting cache and retrying cp=U+%04lx used=%u cap=%u need=%u",
                     (unsigned long)codepoint,
                     (unsigned)_glyph_pool_used,
                     (unsigned)_glyph_pool_capacity,
                     (unsigned)glyph.data_size);
            if (!compactGlyphCache(glyph.data_size)) {
                clearGlyphCache();
            }
            _glyph_pool_full = false;
            if (loadGlyph(codepoint, glyph)) {
                _glyph_cache.push_back(glyph);
                insertLookup(_glyph_lookup, _glyph_cache.back().codepoint, (uint16_t)(_glyph_cache.size() - 1));
                return &_glyph_cache.back();
            }
        }
        if (std::find(_missing_codepoints.begin(), _missing_codepoints.end(), codepoint) == _missing_codepoints.end()) {
            _missing_codepoints.push_back(codepoint);
        }
        return nullptr;
    }

    _glyph_cache.push_back(glyph);
    insertLookup(_glyph_lookup, _glyph_cache.back().codepoint, (uint16_t)(_glyph_cache.size() - 1));
    return &_glyph_cache.back();
}

int TextRenderer::measureCodepoint(uint32_t codepoint)
{
    if (_lv_ready && _lv_font) {
        std::string text = codepointToUtf8(codepoint);
        M5.Display.setTextSize(1);
        return M5.Display.textWidth(text.c_str(), _lv_font);
    }
    if (_ft_ready || _bin_ready) {
        const Advance *adv = getAdvance(codepoint);
        if (adv) return adv->advance;
    }
    return estimate_width_no_font(codepoint, _style.font_size);
}

int TextRenderer::measureCodepointForLayout(uint32_t codepoint)
{
    if (_lv_ready && _lv_font) {
        std::string text = codepointToUtf8(codepoint);
        M5.Display.setTextSize(1);
        int w = M5.Display.textWidth(text.c_str(), _lv_font);
        if (w > 0) return w;
    }
    if (_bin_ready) {
        const Advance *adv = getAdvance(codepoint);
        if (adv) return adv->advance;
        return estimate_width_no_font(codepoint, _bin_font_height ? _bin_font_height : _style.font_size);
    }
    if (_ft_ready) {
        const Advance *adv = getAdvance(codepoint);
        if (adv && adv->advance > 0) return adv->advance;
    }
    if (is_cjk_or_fullwidth(codepoint) || is_cjk_punctuation(codepoint)) return _style.font_size;
    if (codepoint < 0x80) return estimate_width_no_font(codepoint, _style.font_size);
    return estimate_width_no_font(codepoint, _style.font_size);
}

int TextRenderer::measureKerningForLayout(uint32_t left, uint32_t right)
{
    if (!_ft_ready || !_face || left == 0 || right == 0 ||
        left == '\r' || left == '\n' || right == '\r' || right == '\n') {
        return 0;
    }

    FT_Face face = static_cast<FT_Face>(_face);
    if (!FT_HAS_KERNING(face)) return 0;
    FT_UInt left_index = FT_Get_Char_Index(face, static_cast<FT_ULong>(left));
    FT_UInt right_index = FT_Get_Char_Index(face, static_cast<FT_ULong>(right));
    if (left_index == 0 || right_index == 0) return 0;

    FT_Vector delta = {};
    if (FT_Get_Kerning(face, left_index, right_index, FT_KERNING_DEFAULT, &delta) != 0) return 0;
    return static_cast<int>(delta.x >> 6);
}

int TextRenderer::measureFullwidthCellForLayout()
{
    const int fallback = std::max(1, _bin_ready && _bin_font_height
        ? (int)_bin_font_height
        : _style.font_size);
    const uint32_t samples[] = {0x4E00, 0x56FD, 0x6C49}; // Representative CJK cells.
    int measured = 0;
    for (uint32_t cp : samples) {
        int width = measureCodepointForLayout(cp);
        // A missing glyph commonly reports the half-width .notdef advance.
        // Only accept values that plausibly represent a full CJK cell.
        if (width >= fallback * 3 / 4 && width <= fallback * 3 / 2) {
            measured = std::max(measured, width);
        }
    }
    return measured > 0 ? measured : fallback;
}

int TextRenderer::measureUtf8(const std::string &text)
{
    int w = 0;
    int glyph_count = 0;
    uint32_t previous = 0;
    for (size_t pos = 0; pos < text.size();) {
        uint32_t cp = 0;
        pos = nextUtf8(text, pos, cp);
        if (cp == '\r' || cp == '\n') continue;
        if (previous != 0) w += measureKerningForLayout(previous, cp);
        w += measureCodepoint(cp);
        previous = cp;
        glyph_count++;
    }
    if (glyph_count > 1) w += (glyph_count - 1) * _style.letter_spacing;
    return w;
}

void TextRenderer::preloadCommonGlyphs()
{
    // Common CJK chars:
    // 的一是不了在人和有这中大为上个国我以要他时来用们生到作地于出就分对成会可主发
    // 年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力
    // 里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因
    // 由其些然前外天政四日那社义事平形相全
    // Common punctuation:
    // ，。、！？：；“”‘’（）《》〈〉【】——……,.!?:;"'()-
    static const uint32_t kCommonCjk[] = {
        0x7684, 0x4E00, 0x662F, 0x4E0D, 0x4E86, 0x5728, 0x4EBA, 0x548C,
        0x6709, 0x8FD9, 0x4E2D, 0x5927, 0x4E3A, 0x4E0A, 0x4E2A, 0x56FD,
        0x6211, 0x4EE5, 0x8981, 0x4ED6, 0x65F6, 0x6765, 0x7528, 0x4EEC,
        0x751F, 0x5230, 0x4F5C, 0x5730, 0x4E8E, 0x51FA, 0x5C31, 0x5206,
        0x5BF9, 0x6210, 0x4F1A, 0x53EF, 0x4E3B, 0x53D1, 0x5E74, 0x52A8,
        0x540C, 0x5DE5, 0x4E5F, 0x80FD, 0x4E0B, 0x8FC7, 0x5B50, 0x8BF4,
        0x4EA7, 0x79CD, 0x9762, 0x800C, 0x65B9, 0x540E, 0x591A, 0x5B9A,
        0x884C, 0x5B66, 0x6CD5, 0x6240, 0x6C11, 0x5F97, 0x7ECF, 0x5341,
        0x4E09, 0x4E4B, 0x8FDB, 0x7740, 0x7B49, 0x90E8, 0x5EA6, 0x5BB6,
        0x7535, 0x529B, 0x91CC, 0x5982, 0x6C34, 0x5316, 0x9AD8, 0x81EA,
        0x4E8C, 0x7406, 0x8D77, 0x5C0F, 0x7269, 0x73B0, 0x5B9E, 0x52A0,
        0x91CF, 0x90FD, 0x4E24, 0x4F53, 0x5236, 0x673A, 0x5F53, 0x4F7F,
        0x70B9, 0x4ECE, 0x4E1A, 0x672C, 0x53BB, 0x628A, 0x6027, 0x597D,
        0x5E94, 0x5F00, 0x5B83, 0x5408, 0x8FD8, 0x56E0, 0x7531, 0x5176,
        0x4E9B, 0x7136, 0x524D, 0x5916, 0x5929, 0x653F, 0x56DB, 0x65E5,
        0x90A3, 0x793E, 0x4E49, 0x4E8B, 0x5E73, 0x5F62, 0x76F8, 0x5168,
        0xFF0C, 0x3002, 0x3001, 0xFF01, 0xFF1F, 0xFF1A, 0xFF1B, 0x201C,
        0x201D, 0x2018, 0x2019, 0xFF08, 0xFF09, 0x300A, 0x300B, 0x3008,
        0x3009, 0x3010, 0x3011, 0x2014, 0x2026, 0x002C, 0x002E, 0x0021,
        0x003F, 0x003A, 0x003B, 0x0022, 0x0027, 0x0028, 0x0029, 0x002D,
    };

    for (uint32_t cp : kCommonCjk) {
        getAdvance(cp);
    }
}

void TextRenderer::clearGlyphCache()
{
    _glyph_cache.clear();
    _missing_codepoints.clear();
    _page_missing_codepoints.clear();
    _prepared_page_codepoints.clear();
    _glyph_lookup.clear();
    int glyph_limit = std::max(32, _style.glyph_cache_limit);
    _glyph_lookup.assign(next_power_of_two((size_t)glyph_limit * 2), LookupSlot());
    _glyph_pool_used = 0;
    _glyph_pool_full = false;
}

bool TextRenderer::compactGlyphCache(size_t needed_bytes)
{
    if (_glyph_cache.empty() || !_glyph_pool || _glyph_pool_capacity == 0) return false;

    const size_t old_count = _glyph_cache.size();
    const size_t old_used = _glyph_pool_used;
    const size_t keep_bytes_limit = (_glyph_pool_capacity * GLYPH_CACHE_FIFO_RETAIN_PERCENT) / 100;
    const size_t free_target = std::max(needed_bytes, _glyph_pool_capacity / 4);
    const size_t max_used_after = _glyph_pool_capacity > free_target ? _glyph_pool_capacity - free_target : 0;
    const size_t keep_bytes_target = std::min(keep_bytes_limit, max_used_after);
    const size_t keep_count_target = std::max<size_t>(16, ((size_t)std::max(32, _style.glyph_cache_limit) * GLYPH_CACHE_FIFO_RETAIN_COUNT_PERCENT) / 100);

    std::vector<uint8_t> selected(old_count, 0);
    size_t selected_bytes = 0;
    size_t protected_count = 0;

    // Pin all glyphs used by the prepared page. The old FIFO-only policy could
    // evict an old-but-visible glyph while the rest of the same page was still
    // being prepared, which allowed another pool relocation during drawing.
    for (size_t idx = 0; idx < old_count; ++idx) {
        const Glyph &glyph = _glyph_cache[idx];
        if (std::find(_prepared_page_codepoints.begin(), _prepared_page_codepoints.end(),
                      glyph.codepoint) == _prepared_page_codepoints.end()) {
            continue;
        }
        if (glyph.data_size > 0 && glyph.data_offset + glyph.data_size > old_used) continue;
        if (selected_bytes + glyph.data_size > max_used_after) {
            ESP_LOGW(TAG, "glyph compact cannot retain prepared page: protected=%u bytes=%u need=%u cap=%u",
                     (unsigned)protected_count,
                     (unsigned)selected_bytes,
                     (unsigned)needed_bytes,
                     (unsigned)_glyph_pool_capacity);
            return false;
        }
        selected[idx] = 1;
        selected_bytes += glyph.data_size;
        ++protected_count;
    }

    // Add recent entries up to the normal FIFO target. Selection is newest
    // first, but movement below stays in ascending source-offset order.
    size_t selected_count = protected_count;
    for (size_t idx = old_count; idx-- > 0;) {
        if (selected[idx]) continue;
        const Glyph &glyph = _glyph_cache[idx];
        if (glyph.data_size > 0 && glyph.data_offset + glyph.data_size > old_used) continue;
        if (selected_count >= keep_count_target) break;
        if (selected_bytes + glyph.data_size > keep_bytes_target) continue;
        selected[idx] = 1;
        selected_bytes += glyph.data_size;
        ++selected_count;
    }

    std::vector<Glyph> kept;
    kept.reserve(selected_count);
    size_t new_used = 0;
    for (size_t idx = 0; idx < old_count; ++idx) {
        if (!selected[idx]) continue;
        const Glyph &glyph = _glyph_cache[idx];

        Glyph moved = glyph;
        if (moved.data_size > 0) {
            std::memmove(_glyph_pool + new_used, _glyph_pool + moved.data_offset, moved.data_size);
            moved.data_offset = new_used;
            new_used += moved.data_size;
        } else {
            moved.data_offset = 0;
        }
        kept.push_back(moved);
    }

    if (kept.empty() && needed_bytes > 0) return false;

    _glyph_cache.swap(kept);
    _glyph_pool_used = new_used;
    _glyph_pool_full = false;
    _glyph_lookup.clear();
    int glyph_limit = std::max(32, _style.glyph_cache_limit);
    _glyph_lookup.assign(next_power_of_two((size_t)glyph_limit * 2), LookupSlot());
    for (size_t i = 0; i < _glyph_cache.size(); ++i) {
        insertLookup(_glyph_lookup, _glyph_cache[i].codepoint, (uint16_t)i);
    }

    ESP_LOGI(TAG, "glyph cache compacted count=%u/%u protected=%u pool=%u/%u need=%u",
             (unsigned)_glyph_cache.size(),
             (unsigned)old_count,
             (unsigned)protected_count,
             (unsigned)_glyph_pool_used,
             (unsigned)_glyph_pool_capacity,
             (unsigned)needed_bytes);
    return _glyph_pool_capacity >= _glyph_pool_used + needed_bytes;
}

bool TextRenderer::preparePage(const std::string &text)
{
    if (_lv_ready) {
        return true;
    }
    if (!_ft_ready && !_bin_ready) {
        ESP_LOGE(TAG, "preparePage failed: font not ready");
        return false;
    }
    _page_missing_codepoints.clear();
    if (_perf) *_perf = PerfCounters();
    FontStreamContext *font_ctx = (FontStreamContext *)_font_stream_ctx;
    reset_font_stream_perf(font_ctx);

    std::vector<uint32_t> cps;
    cps.reserve(256);
    for (size_t pos = 0; pos < text.size();) {
        uint32_t cp = 0;
        pos = nextUtf8(text, pos, cp);
        if (cp == '\r' || cp == '\n') continue;
        if (std::find(cps.begin(), cps.end(), cp) == cps.end()) cps.push_back(cp);
    }

    _prepared_page_codepoints = cps;

    // Make room before glyph loading starts so pool relocation happens at a
    // page boundary, never halfway through the following draw pass.
    size_t missing_count = 0;
    for (uint32_t cp : cps) {
        if (findLookup(_glyph_lookup, cp) < 0 &&
            std::find(_missing_codepoints.begin(), _missing_codepoints.end(), cp) == _missing_codepoints.end()) {
            ++missing_count;
        }
    }
    const size_t glyph_side = (size_t)std::max(16, _style.font_size + 12);
    const size_t estimated_per_glyph = (glyph_side * glyph_side + 1) / 2 + 64;
    const size_t estimated_need = std::min(_glyph_pool_capacity / 2,
                                           missing_count * estimated_per_glyph);
    if (missing_count > 0 &&
        _glyph_pool_used + estimated_need > (_glyph_pool_capacity * 9) / 10) {
        ESP_LOGI(TAG, "glyph cache page-boundary compact: glyphs=%u missing=%u used=%u cap=%u reserve=%u",
                 (unsigned)cps.size(),
                 (unsigned)missing_count,
                 (unsigned)_glyph_pool_used,
                 (unsigned)_glyph_pool_capacity,
                 (unsigned)estimated_need);
        if (!compactGlyphCache(estimated_need)) {
            clearGlyphCache();
            _prepared_page_codepoints = cps;
        }
    }

    int64_t t0 = esp_timer_get_time();
    bool ok = true;
    size_t loaded_before = _glyph_cache.size();

    // Loading neighboring glyph indices together improves locality in the
    // font's outline tables. This changes only the preload order; the second
    // pass below restores page-order LRU usage and performs no extra I/O.
    if (_ft_ready && _face) {
        struct PendingGlyph {
            uint32_t codepoint;
            FT_UInt glyph_index;
        };
        std::vector<PendingGlyph> pending;
        pending.reserve(cps.size());
        FT_Face face = (FT_Face)_face;
        for (uint32_t cp : cps) {
            if (findLookup(_glyph_lookup, cp) >= 0) continue;
            if (std::find(_missing_codepoints.begin(), _missing_codepoints.end(), cp) != _missing_codepoints.end()) continue;
            pending.push_back({cp, FT_Get_Char_Index(face, (FT_ULong)cp)});
        }
        std::stable_sort(pending.begin(), pending.end(), [](const PendingGlyph &a, const PendingGlyph &b) {
            return a.glyph_index < b.glyph_index;
        });
        for (const PendingGlyph &item : pending) getGlyph(item.codepoint);
    }

    for (uint32_t cp : cps) {
        if (!getGlyph(cp)) {
            if (std::find(_page_missing_codepoints.begin(), _page_missing_codepoints.end(), cp) == _page_missing_codepoints.end()) {
                _page_missing_codepoints.push_back(cp);
            }
            ok = false;
        }
    }
    int64_t t1 = esp_timer_get_time();
    size_t loaded_after = _glyph_cache.size();
    unsigned new_loaded = loaded_after >= loaded_before ? (unsigned)(loaded_after - loaded_before) : (unsigned)loaded_after;
    ESP_LOGI(TAG, "preparePage glyphs=%u new=%u cached=%u missing=%u pool=%u/%u bpp=%d time=%lld ms internal_free=%u largest=%u spiram_free=%u",
             (unsigned)cps.size(),
             new_loaded,
             (unsigned)_glyph_cache.size(),
             (unsigned)_page_missing_codepoints.size(),
             (unsigned)_glyph_pool_used,
             (unsigned)_glyph_pool_capacity,
             GLYPH_BITMAP_BPP,
             (t1 - t0) / 1000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "perf prepare io stream_calls=%u cache_hits=%u resident_hits=%u misses=%u hit_bytes=%u physical_reads=%u read_bytes=%u read_time=%lld us direct=%u fills=%u",
             font_ctx ? (unsigned)font_ctx->stream_calls : 0,
             font_ctx ? (unsigned)font_ctx->cache_hits : 0,
             font_ctx ? (unsigned)font_ctx->resident_hits : 0,
             font_ctx ? (unsigned)font_ctx->cache_misses : 0,
             font_ctx ? (unsigned)font_ctx->cache_hit_bytes : 0,
             font_ctx ? (unsigned)font_ctx->physical_reads : 0,
             font_ctx ? (unsigned)font_ctx->physical_read_bytes : 0,
             font_ctx ? font_ctx->physical_read_us : 0,
             font_ctx ? (unsigned)font_ctx->direct_reads : 0,
             font_ctx ? (unsigned)font_ctx->fill_reads : 0);
    ESP_LOGI(TAG, "perf prepare ft glyph_loads=%u glyph_total=%lld us load_char=%lld us bbox=%lld us clear=%lld us raster=%lld us advance_loads=%u advance_load=%lld us slowest=U+%04lx/%lld us",
             _perf ? (unsigned)_perf->glyph_loads : 0,
             _perf ? _perf->glyph_total_us : 0,
             _perf ? _perf->glyph_load_char_us : 0,
             _perf ? _perf->glyph_bbox_us : 0,
             _perf ? _perf->glyph_clear_us : 0,
             _perf ? _perf->glyph_raster_us : 0,
             _perf ? (unsigned)_perf->advance_loads : 0,
             _perf ? _perf->advance_load_us : 0,
             (unsigned long)(_perf ? _perf->slowest_glyph_cp : 0),
             _perf ? _perf->slowest_glyph_us : 0);
    if (_bin_ready) {
        ESP_LOGI(TAG, "perf prepare bin encoding=%s glyph_loads=%u entry_reads=%u entry=%lld us bitmap_reads=%u bitmap_bytes=%lu bitmap_read=%lld us decode=%lld us total=%lld us slowest=U+%04lx/%lld us",
                 _bin_encoding == BIN_FONT_ENCODING_RAW_4BPP ? "raw4bpp" : "edc-bitstream",
                 _perf ? (unsigned)_perf->bin_glyph_loads : 0,
                 _perf ? (unsigned)_perf->bin_entry_reads : 0,
                 _perf ? _perf->bin_entry_us : 0,
                 _perf ? (unsigned)_perf->bin_bitmap_reads : 0,
                 _perf ? (unsigned long)_perf->bin_bitmap_bytes : 0,
                 _perf ? _perf->bin_bitmap_read_us : 0,
                 _perf ? _perf->bin_decode_us : 0,
                 _perf ? _perf->bin_total_us : 0,
                 (unsigned long)(_perf ? _perf->slowest_glyph_cp : 0),
                 _perf ? _perf->slowest_glyph_us : 0);
    }
    return ok;
}

std::vector<text_layout::Line> TextRenderer::layoutUtf8Paragraph(const std::string &text,
                                                                 int max_width,
                                                                 int first_line_indent)
{
    text_layout::Options options;
    options.max_width = max_width;
    options.letter_spacing = _style.letter_spacing;
    int explicit_indent = 0;
    for (size_t pos = 0; pos < text.size();) {
        uint32_t cp = 0;
        size_t next = nextUtf8(text, pos, cp);
        if (cp != 0x3000) break;
        explicit_indent += measureCodepointForLayout(cp);
        pos = next;
    }
    // Preserve indentation already present in the document and only add the
    // missing width needed to reach the configured full-width cell count.
    options.first_line_indent = std::max(0, first_line_indent - explicit_indent);
    options.prefer_word_break = true;
    options.apply_cjk_kinsoku = true;
    auto measure = [](void *context, uint32_t codepoint) -> int {
        return static_cast<TextRenderer *>(context)->measureCodepointForLayout(codepoint);
    };
    auto measure_kerning = [](void *context, uint32_t left, uint32_t right) -> int {
        return static_cast<TextRenderer *>(context)->measureKerningForLayout(left, right);
    };
    return text_layout::break_utf8(text, options, measure, this, measure_kerning);
}

int TextRenderer::verticalCellAdvance() const
{
    return std::max(1, _style.font_size + _style.letter_spacing);
}

int TextRenderer::verticalRubyLaneWidth() const
{
    // Ruby reuses the normal-size cached glyph and is reduced only at draw time.
    return std::max(4, (verticalCellAdvance() + 1) / 2 + 4);
}

int TextRenderer::measureUtf8Vertical(const std::string &text) const
{
    int cells = 0;
    for (size_t pos = 0; pos < text.size();) {
        uint32_t cp = 0;
        pos = nextUtf8(text, pos, cp);
        if (cp != '\r' && cp != '\n') ++cells;
    }
    return cells * verticalCellAdvance();
}

int TextRenderer::measureUtf8VerticalStyled(
    const std::string &text, const std::vector<inline_style::Span> &styles,
    size_t byte_end) const
{
    const int cell = verticalCellAdvance();
    const int small_cell = std::max(1, (cell * 65 + 50) / 100);
    int height = 0;
    const size_t limit = std::min(byte_end, text.size());
    size_t style_index = 0;
    for (size_t pos = 0; pos < limit;) {
        const size_t byte_pos = pos;
        uint32_t cp = 0;
        pos = nextUtf8(text, pos, cp);
        if (cp == '\r' || cp == '\n') continue;
        while (style_index < styles.size() && styles[style_index].byte_end <= byte_pos)
            ++style_index;
        bool small = false;
        for (size_t si = style_index; si < styles.size(); ++si) {
            if (styles[si].byte_start > byte_pos) break;
            if (byte_pos < styles[si].byte_end &&
                inline_style::has(styles[si].flags, inline_style::Small)) small = true;
        }
        height += small ? small_cell : cell;
    }
    return height;
}

std::vector<text_layout::Line> TextRenderer::layoutUtf8VerticalParagraph(
    const std::string &text, int max_height, int first_column_indent)
{
    text_layout::Options options;
    options.max_width = max_height;
    options.first_line_indent = first_column_indent;
    options.prefer_word_break = false;
    options.apply_cjk_kinsoku = true;
    options.optimize_western_lines = false;
    options.hyphenate_english = false;
    auto measure = [](void *context, uint32_t) -> int {
        return static_cast<TextRenderer *>(context)->verticalCellAdvance();
    };
    return text_layout::break_utf8(text, options, measure, this);
}

std::vector<text_layout::Line> TextRenderer::layoutUtf8VerticalParagraphStyled(
    const std::string &text, const std::vector<inline_style::Span> &styles,
    int max_height, int first_column_indent)
{
    text_layout::Options options;
    options.max_width = max_height;
    options.first_line_indent = first_column_indent;
    options.prefer_word_break = false;
    options.apply_cjk_kinsoku = true;
    options.optimize_western_lines = false;
    options.hyphenate_english = false;
    struct Context {
        TextRenderer *renderer;
        const std::vector<inline_style::Span> *styles;
    } context{this, &styles};
    auto measure = [](void *ctx, uint32_t) -> int {
        return static_cast<Context *>(ctx)->renderer->verticalCellAdvance();
    };
    auto measure_at = [](void *ctx, uint32_t, size_t byte_pos) -> int {
        auto *state = static_cast<Context *>(ctx);
        const auto &spans = *state->styles;
        const int cell = state->renderer->verticalCellAdvance();
        for (const auto &span : spans) {
            if (span.byte_start > byte_pos) break;
            if (byte_pos < span.byte_end &&
                inline_style::has(span.flags, inline_style::Small))
                return std::max(1, (cell * 65 + 50) / 100);
        }
        return cell;
    };
    return text_layout::break_utf8(text, options, measure, &context, nullptr, measure_at);
}

void TextRenderer::drawStyledUtf8VerticalColumn(
    lgfx::LGFXBase &gfx, const std::string &text,
    const std::vector<inline_style::Span> &styles,
    int x, int y, int column_width, uint16_t color)
{
    const int cell = verticalCellAdvance();
    size_t style_index = 0;
    for (size_t pos = 0; pos < text.size();) {
        const size_t byte_pos = pos;
        uint32_t cp = 0;
        pos = nextUtf8(text, pos, cp);
        if (cp == '\r' || cp == '\n') continue;
        while (style_index < styles.size() && styles[style_index].byte_end <= byte_pos)
            ++style_index;
        uint8_t flags = inline_style::None;
        for (size_t si = style_index; si < styles.size(); ++si) {
            if (styles[si].byte_start > byte_pos) break;
            if (byte_pos < styles[si].byte_end) flags |= styles[si].flags;
        }
        const bool small = inline_style::has(flags, inline_style::Small);
        const int scale = small ? 65 : 100;
        if (_lv_ready && _lv_font) {
            const std::string glyph_text = codepointToUtf8(cp);
            gfx.setTextSize(1);
            gfx.setTextDatum(textdatum_t::top_left);
            gfx.setTextColor(color, color);
            gfx.drawString(glyph_text.c_str(), x, y, _lv_font);
        } else {
            // Unicode vertical presentation forms keep punctuation in the
            // correct corner without rotating the entire framebuffer. If a
            // font lacks one, retain its ordinary glyph rather than a box.
            uint32_t vertical_cp = 0;
            switch (cp) {
            case 0xff0c: vertical_cp = 0xfe10; break; // ，
            case 0x3001: vertical_cp = 0xfe11; break; // 、
            case 0x3002: vertical_cp = 0xfe12; break; // 。
            case 0xff1a: vertical_cp = 0xfe13; break; // ：
            case 0xff1b: vertical_cp = 0xfe14; break; // ；
            case 0xff01: vertical_cp = 0xfe15; break; // ！
            case 0xff1f: vertical_cp = 0xfe16; break; // ？
            case 0xff08: vertical_cp = 0xfe35; break; // （
            case 0xff09: vertical_cp = 0xfe36; break; // ）
            case 0x300a: vertical_cp = 0xfe3d; break; // 《
            case 0x300b: vertical_cp = 0xfe3e; break; // 》
            case 0x300c: vertical_cp = 0xfe41; break; // 「
            case 0x300d: vertical_cp = 0xfe42; break; // 」
            case 0x2026: vertical_cp = 0xfe19; break; // …
            default: break;
            }
            const Glyph *glyph = vertical_cp ? getGlyph(vertical_cp) : nullptr;
            if (!glyph) glyph = getGlyph(cp);
            if (glyph) {
                const int advance = std::max(1, (glyph->advance * scale + 50) / 100);
                const int glyph_x = x + std::max(0, (column_width - advance) / 2);
                if (small) {
                    drawGlyphScaled(gfx, *glyph, glyph_x,
                                    y + (baseline() * scale + 50) / 100,
                                    scale, color);
                } else {
                    drawGlyphStyled(gfx, *glyph, glyph_x, y + baseline(), color, flags);
                }
            } else {
                gfx.drawRect(x, y, std::min(column_width, cell),
                             small ? std::max(1, (cell * 65 + 50) / 100) : cell, color);
            }
        }
        y += small ? std::max(1, (cell * 65 + 50) / 100) : cell;
    }
}

bool TextRenderer::drawUtf8VerticalRuby(lgfx::LGFXBase &gfx, const std::string &text,
                                        int x, int y, int base_height, int lane_width,
                                        uint16_t color)
{
    if ((!_ft_ready && !_bin_ready) || text.empty() || base_height <= 0 || lane_width <= 0)
        return false;
    int count = 0;
    for (size_t pos = 0; pos < text.size();) {
        uint32_t cp = 0;
        pos = nextUtf8(text, pos, cp);
        if (cp != '\r' && cp != '\n') ++count;
    }
    if (count == 0) return false;
    const int cell = verticalCellAdvance();
    const int scale = std::min(50, base_height * 100 / std::max(1, count * cell));
    // Exceptionally long readings remain available through the popup instead
    // of colliding with the following base character's ruby.
    if (scale < 30) return false;
    const int small_cell = std::max(1, (cell * scale + 99) / 100);
    const int total_height = count * small_cell;
    int cursor_y = y + std::max(0, (base_height - total_height) / 2);
    for (size_t pos = 0; pos < text.size();) {
        uint32_t cp = 0;
        pos = nextUtf8(text, pos, cp);
        if (cp == '\r' || cp == '\n') continue;
        const Glyph *glyph = getGlyph(cp);
        if (!glyph) return false;
        const int advance = std::max(1, (glyph->advance * scale + 50) / 100);
        const int glyph_x = x + std::max(0, (lane_width - advance) / 2);
        const int small_baseline = (baseline() * scale + 50) / 100;
        drawGlyphScaled(gfx, *glyph, glyph_x, cursor_y + small_baseline,
                        scale, color);
        cursor_y += small_cell;
    }
    return true;
}

std::vector<std::string> TextRenderer::wrapUtf8Line(const std::string &line, int max_width)
{
    std::vector<text_layout::Line> layout = layoutUtf8Paragraph(line, max_width);
    std::vector<std::string> lines;
    lines.reserve(layout.size());
    for (auto &item : layout) lines.push_back(std::move(item.text));
    return lines;
}

void TextRenderer::drawGlyph(lgfx::LGFXBase &gfx, const Glyph &glyph,
                             int x, int baseline_y, uint16_t color)
{
    if (glyph.data_size == 0 || glyph.width <= 0 || glyph.height <= 0) return;
    if (glyph.data_offset + glyph.data_size > _glyph_pool_used) return;

    int gx = x + glyph.bitmap_left;
    int gy = baseline_y - glyph.bitmap_top;
    if (!USE_GLYPH_PUSH_GRAYIMAGE) {
        drawGlyphRuns(gfx, glyph, x, baseline_y, color);
        return;
    }
    (void)color;
    gfx.pushGrayscaleImage(gx,
                           gy,
                           glyph.width,
                           glyph.height,
                           _glyph_pool + glyph.data_offset,
                           lgfx::color_depth_t::grayscale_4bit,
                           0x000000,
                           0xFFFFFF);
}

void TextRenderer::drawGlyphScaled(lgfx::LGFXBase &gfx, const Glyph &glyph,
                                   int x, int baseline_y, int scale_percent, uint16_t color)
{
    if (glyph.data_size == 0 || glyph.width <= 0 || glyph.height <= 0 ||
        glyph.data_offset + glyph.data_size > _glyph_pool_used) return;
    const int dst_w = std::max(1, (glyph.width * scale_percent + 50) / 100);
    const int dst_h = std::max(1, (glyph.height * scale_percent + 50) / 100);
    const int gx = x + (glyph.bitmap_left * scale_percent + 50) / 100;
    const int gy = baseline_y - (glyph.bitmap_top * scale_percent + 50) / 100;
    const uint8_t *bitmap = _glyph_pool + glyph.data_offset;
    for (int row = 0; row < dst_h; ++row) {
        const int sy0 = row * glyph.height / dst_h;
        const int sy1 = std::max(sy0 + 1, (row + 1) * glyph.height / dst_h);
        int run_start = -1;
        uint8_t run_level = 0;
        auto flush = [&](int end) {
            if (run_start < 0) return;
            const uint8_t gray = color == TFT_WHITE ? run_level * 17
                : static_cast<uint8_t>(255 - run_level * 17);
            const uint16_t draw_color = (color == TFT_BLACK || color == TFT_WHITE)
                ? gray565(gray) : color;
            gfx.writeFastHLine(gx + run_start, gy + row, end - run_start, draw_color);
            run_start = -1;
        };
        for (int col = 0; col < dst_w; ++col) {
            const int sx0 = col * glyph.width / dst_w;
            const int sx1 = std::max(sx0 + 1, (col + 1) * glyph.width / dst_w);
            int sum = 0;
            int samples = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t *src = bitmap + sy * glyph.pitch;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint8_t packed = src[sx >> 1];
                    sum += (sx & 1) ? packed & 0x0f : packed >> 4;
                    ++samples;
                }
            }
            const uint8_t level = static_cast<uint8_t>((sum + samples / 2) / samples);
            if (level == 0) {
                flush(col);
            } else if (run_start < 0) {
                run_start = col;
                run_level = level;
            } else if (level != run_level) {
                flush(col);
                run_start = col;
                run_level = level;
            }
        }
        flush(dst_w);
    }
}

void TextRenderer::drawGlyphRuns(lgfx::LGFXBase &gfx, const Glyph &glyph, int x, int baseline_y, uint16_t color)
{
    if (glyph.data_size == 0 || glyph.width <= 0 || glyph.height <= 0) return;
    if (glyph.data_offset + glyph.data_size > _glyph_pool_used) return;

    int gx = x + glyph.bitmap_left;
    int gy = baseline_y - glyph.bitmap_top;
    for (int row = 0; row < glyph.height; ++row) {
        const uint8_t *src = _glyph_pool + glyph.data_offset + row * glyph.pitch;
        int run_start = -1;
        uint8_t run_level = 0;
        for (int col = 0; col < glyph.width; ++col) {
            uint8_t packed = src[col >> 1];
            uint8_t level = (col & 1) ? (packed & 0x0F) : (packed >> 4);

            if (level == 0) {
                if (run_start >= 0) {
                    uint8_t v = color == TFT_WHITE ? (uint8_t)(run_level * 17) : (uint8_t)(255 - run_level * 17);
                    uint16_t draw_color = (color == TFT_BLACK || color == TFT_WHITE) ? gray565(v) : color;
                    gfx.writeFastHLine(gx + run_start, gy + row, col - run_start, draw_color);
                    run_start = -1;
                }
                continue;
            }

            if (run_start < 0) {
                run_start = col;
                run_level = level;
            } else if (level != run_level) {
                uint8_t v = color == TFT_WHITE ? (uint8_t)(run_level * 17) : (uint8_t)(255 - run_level * 17);
                uint16_t draw_color = (color == TFT_BLACK || color == TFT_WHITE) ? gray565(v) : color;
                gfx.writeFastHLine(gx + run_start, gy + row, col - run_start, draw_color);
                run_start = col;
                run_level = level;
            }
        }
        if (run_start >= 0) {
            uint8_t v = color == TFT_WHITE ? (uint8_t)(run_level * 17) : (uint8_t)(255 - run_level * 17);
            uint16_t draw_color = (color == TFT_BLACK || color == TFT_WHITE) ? gray565(v) : color;
            gfx.writeFastHLine(gx + run_start, gy + row, glyph.width - run_start, draw_color);
        }
    }
}

void TextRenderer::drawUtf8Line(lgfx::LGFXBase &gfx, const std::string &text, int x, int y,
                                uint16_t color, int justify_width)
{
    static const std::vector<inline_style::Span> no_styles;
    drawStyledUtf8Line(gfx, text, no_styles, x, y, color, justify_width);
}

void TextRenderer::drawGlyphStyled(lgfx::LGFXBase &gfx, const Glyph &glyph,
                                   int x, int baseline_y, uint16_t color,
                                   uint8_t style_flags)
{
    if (!inline_style::has(style_flags, inline_style::Bold) &&
        !inline_style::has(style_flags, inline_style::Italic)) {
        drawGlyph(gfx, glyph, x, baseline_y, color);
        return;
    }
    if (glyph.data_size == 0 || glyph.width <= 0 || glyph.height <= 0) return;
    if (glyph.data_offset + glyph.data_size > _glyph_pool_used) return;

    const int gx = x + glyph.bitmap_left;
    const int gy = baseline_y - glyph.bitmap_top;
    const int bold_px = inline_style::has(style_flags, inline_style::Bold) ? 1 : 0;
    const int italic_px = inline_style::has(style_flags, inline_style::Italic)
                        ? std::max(2, std::min(5, _style.font_size / 10)) : 0;
    for (int row = 0; row < glyph.height; ++row) {
        const uint8_t *src = _glyph_pool + glyph.data_offset + row * glyph.pitch;
        const int shear = italic_px > 0
                        ? ((glyph.height - 1 - row) * italic_px) / std::max(1, glyph.height - 1)
                        : 0;
        int run_start = -1;
        uint8_t run_level = 0;
        auto flush_run = [&](int end) {
            if (run_start < 0) return;
            const uint8_t v = color == TFT_WHITE ? (uint8_t)(run_level * 17)
                                                  : (uint8_t)(255 - run_level * 17);
            const uint16_t draw_color = (color == TFT_BLACK || color == TFT_WHITE) ? gray565(v) : color;
            gfx.writeFastHLine(gx + run_start + shear, gy + row,
                               end - run_start + bold_px, draw_color);
            run_start = -1;
        };
        for (int col = 0; col < glyph.width; ++col) {
            const uint8_t packed = src[col >> 1];
            const uint8_t level = (col & 1) ? (packed & 0x0F) : (packed >> 4);
            if (level == 0) {
                flush_run(col);
            } else if (run_start < 0) {
                run_start = col;
                run_level = level;
            } else if (level != run_level) {
                flush_run(col);
                run_start = col;
                run_level = level;
            }
        }
        flush_run(glyph.width);
    }
}

void TextRenderer::drawStyledUtf8Line(lgfx::LGFXBase &gfx, const std::string &text,
                                      const std::vector<inline_style::Span> &styles,
                                      int x, int y, uint16_t color, int justify_width)
{
    if (!_ft_ready && !_bin_ready && !_lv_ready) {
        ESP_LOGE(TAG, "drawUtf8Line skipped: font not ready");
        return;
    }

    if (_lv_ready && _lv_font) {
        gfx.setTextSize(1);
        gfx.setTextDatum(textdatum_t::top_left);
        gfx.setTextColor(color, color);
        gfx.drawString(text.c_str(), x, y, _lv_font);
        return;
    }

    int cursor = x;
    int baseline_y = y + baseline();
    std::vector<uint32_t> codepoints;
    codepoints.reserve(text.size() / 2 + 1);
    int cjk_count = 0;
    for (size_t count_pos = 0; count_pos < text.size();) {
        uint32_t cp = 0;
        count_pos = nextUtf8(text, count_pos, cp);
        if (cp != '\r' && cp != '\n') {
            codepoints.push_back(cp);
            if (is_cjk_or_fullwidth(cp) || is_cjk_punctuation(cp)) ++cjk_count;
        }
    }
    const int glyph_count = (int)codepoints.size();
    std::vector<int> gap_extra(glyph_count > 1 ? (size_t)glyph_count - 1 : 0, 0);
    if (justify_width > 0 && glyph_count > 1) {
        int natural_width = measureUtf8(text);
        int requested_extra = std::max(0, justify_width - natural_width);
        std::vector<int> word_gaps;
        std::vector<int> cjk_gaps;
        word_gaps.reserve((size_t)glyph_count / 4 + 1);
        cjk_gaps.reserve((size_t)glyph_count - 1);
        for (int i = 0; i + 1 < glyph_count; ++i) {
            const uint32_t left = codepoints[(size_t)i];
            const uint32_t right = codepoints[(size_t)i + 1];
            if ((left == ' ' || left == '\t') && right != ' ' && right != '\t') {
                word_gaps.push_back(i);
            }

            if (cjk_count * 2 < glyph_count) continue;
            const bool left_cjk = is_cjk_or_fullwidth(left) || is_cjk_punctuation(left);
            const bool right_cjk = is_cjk_or_fullwidth(right) || is_cjk_punctuation(right);
            const bool whitespace = left == ' ' || left == '\t' || right == ' ' || right == '\t';
            const bool paired_mark = (left == right) &&
                                     (left == 0x2014 || left == 0x2025 || left == 0x2026);
            if (left_cjk && right_cjk && !whitespace && !paired_mark &&
                !text_layout::prohibited_at_line_end(left) &&
                !text_layout::prohibited_at_line_start(right)) {
                cjk_gaps.push_back(i);
            }
        }

        auto distribute = [&gap_extra, &requested_extra](const std::vector<int> &gaps,
                                                          int max_per_gap) {
            if (gaps.empty() || requested_extra <= 0) return;
            const int distributed = std::min(requested_extra,
                                             max_per_gap * (int)gaps.size());
            const int per_gap = distributed / (int)gaps.size();
            const int remainder = distributed % (int)gaps.size();
            for (size_t i = 0; i < gaps.size(); ++i) {
                gap_extra[(size_t)gaps[i]] += per_gap + ((int)i < remainder ? 1 : 0);
            }
            requested_extra -= distributed;
        };

        // Mixed lines prefer word separators. Individual Latin glyphs are
        // never spread; CJK gaps absorb only the remaining adjustment.
        distribute(word_gaps, std::max(2, measureCodepointForLayout(' ')));
        distribute(cjk_gaps, std::max(1, _style.font_size / 4));
    }
    int glyph_index = 0;
    uint32_t previous_cp = 0;
    size_t style_index = 0;
    for (size_t pos = 0; pos < text.size();) {
        const size_t byte_pos = pos;
        uint32_t cp = 0;
        pos = nextUtf8(text, pos, cp);
        if (cp == '\r' || cp == '\n') continue;

        if (previous_cp != 0) cursor += measureKerningForLayout(previous_cp, cp);

        const Glyph *glyph = getGlyph(cp);
        if (!glyph) {
            int w = measureCodepoint(cp);
            gfx.drawRect(cursor, y, std::max(4, w), std::max(4, lineHeight() - _style.line_spacing), color);
            cursor += w;
            if (glyph_index + 1 < glyph_count) {
                cursor += _style.letter_spacing + gap_extra[(size_t)glyph_index];
            }
            ++glyph_index;
            previous_cp = cp;
            continue;
        }

        while (style_index < styles.size() && styles[style_index].byte_end <= byte_pos) {
            ++style_index;
        }
        uint8_t style_flags = inline_style::None;
        for (size_t si = style_index; si < styles.size(); ++si) {
            if (styles[si].byte_start > byte_pos) break;
            if (byte_pos < styles[si].byte_end) style_flags |= styles[si].flags;
        }
        drawGlyphStyled(gfx, *glyph, cursor, baseline_y, color, style_flags);
        cursor += glyph->advance;
        if (glyph_index + 1 < glyph_count) {
            cursor += _style.letter_spacing + gap_extra[(size_t)glyph_index];
        }
        ++glyph_index;
        previous_cp = cp;
    }
}

TextRenderStats TextRenderer::stats() const
{
    TextRenderStats s;
    s.glyph_count = _glyph_cache.size();
    s.glyph_pool_used = _glyph_pool_used;
    s.glyph_pool_capacity = _glyph_pool_capacity;
    s.advance_count = _advance_cache.size();
    s.missing_glyphs = (uint32_t)_page_missing_codepoints.size();
    return s;
}

std::string TextRenderer::layoutSignature() const
{
    struct stat st = {};
    long file_size = -1;
    long file_mtime = -1;
    if (_style.font_path && _style.font_path[0] && stat(_style.font_path, &st) == 0) {
        file_size = (long)st.st_size;
        file_mtime = (long)st.st_mtime;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "text_algo=%lu;ft=%d;bin=%d;lv=%d;path=%s;size=%d;bin_height=%u;line=%d;letter=%d;bpp=%d;pool=%d;file_size=%ld;mtime=%ld",
                  (unsigned long)text_layout::ALGORITHM_VERSION,
                  _ft_ready ? 1 : 0,
                  _bin_ready ? 1 : 0,
                  _lv_ready ? 1 : 0,
                  _style.font_path ? _style.font_path : "",
                  _style.font_size,
                  (unsigned)_bin_font_height,
                  _style.line_spacing,
                  _style.letter_spacing,
                  GLYPH_BITMAP_BPP,
                  _style.glyph_pool_bytes,
                  file_size,
                  file_mtime);
    return buf;
}
