#include "map/AmapImageProcessor.h"

#include "lgfx/utility/lgfx_pngle.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *TAG = "AMAP_IMAGE";
constexpr uint8_t CONTRAST_BLACK_POINT = 100;
constexpr uint8_t CONTRAST_WHITE_POINT = 232;

struct DecodeContext {
    FILE *file = nullptr;
    uint8_t *frame = nullptr;
    uint16_t source_width = 0;
    uint16_t source_height = 0;
    uint16_t output_width = 0;
    uint16_t output_height = 0;
    uint16_t output_stride = 0;
    uint8_t integer_scale = 0;
    const uint8_t *gray_lut = nullptr;
    bool bounds_error = false;
    size_t pixels = 0;
};

std::array<uint8_t, 256> make_contrast_gray4_lut()
{
    std::array<uint8_t, 256> lut = {};
    constexpr int range = CONTRAST_WHITE_POINT - CONTRAST_BLACK_POINT;
    for (int value = 0; value < 256; ++value) {
        int adjusted = 0;
        if (value >= CONTRAST_WHITE_POINT) {
            adjusted = 255;
        } else if (value > CONTRAST_BLACK_POINT) {
            adjusted = (value - CONTRAST_BLACK_POINT) * 255 / range;
        }
        lut[static_cast<size_t>(value)] =
            static_cast<uint8_t>(std::min(15, (adjusted + 8) / 17));
    }
    return lut;
}

uint32_t png_read(void *user_data, uint8_t *buffer, uint32_t length)
{
    auto *context = static_cast<DecodeContext *>(user_data);
    if (!context || !context->file) return 0;
    // lgfx_pngle passes a null destination when it wants the data wrapper to
    // skip an ancillary chunk. Treat that as a seek, not as an invalid read.
    if (!buffer) {
        return std::fseek(context->file, static_cast<long>(length), SEEK_CUR) == 0
                   ? length
                   : 0;
    }
    return static_cast<uint32_t>(std::fread(buffer, 1, length, context->file));
}

inline void write_gray4(uint8_t *row, uint16_t x, uint8_t gray)
{
    uint8_t &packed = row[x >> 1];
    if (x & 1) packed = static_cast<uint8_t>((packed & 0xF0) | gray);
    else packed = static_cast<uint8_t>((packed & 0x0F) | (gray << 4));
}

void png_draw(void *user_data, uint32_t x, uint32_t y,
              uint_fast8_t div_x, size_t length, const uint8_t *argb)
{
    auto *context = static_cast<DecodeContext *>(user_data);
    if (!context || !context->frame || !context->gray_lut || !argb ||
        y >= context->source_height || div_x == 0) {
        if (context) context->bounds_error = true;
        return;
    }

    for (size_t i = 0; i < length; ++i, argb += 4) {
        const uint32_t source_x = x + static_cast<uint32_t>(i) * div_x;
        if (source_x >= context->source_width) {
            context->bounds_error = true;
            continue;
        }

        const uint32_t alpha = argb[0];
        const uint32_t luminance =
            (77u * argb[1] + 150u * argb[2] + 29u * argb[3] + 128u) >> 8;
        const uint32_t on_white =
            (luminance * alpha + 255u * (255u - alpha) + 127u) / 255u;
        const uint8_t gray = context->gray_lut[on_white];

        if (context->integer_scale == 2) {
            // Two identical output pixels occupy one complete 4bpp byte.
            const uint8_t pair = static_cast<uint8_t>((gray << 4) | gray);
            const size_t row0 = size_t(y * 2) * context->output_stride;
            const size_t row1 = row0 + context->output_stride;
            context->frame[row0 + source_x] = pair;
            context->frame[row1 + source_x] = pair;
        } else {
            uint8_t *row = context->frame + size_t(y) * context->output_stride;
            write_gray4(row, static_cast<uint16_t>(source_x), gray);
        }
        ++context->pixels;
    }
}

} // namespace

AmapGrayFrame::~AmapGrayFrame()
{
    reset();
}

void AmapGrayFrame::reset()
{
    if (_data) heap_caps_free(_data);
    _data = nullptr;
    _width = 0;
    _height = 0;
    _nearest_scale = 0;
}

bool AmapGrayFrame::load_png(const char *path,
                             uint16_t output_width,
                             uint16_t output_height)
{
    if (!path || !path[0] || output_width == 0 || output_height == 0) return false;
    const int64_t started = esp_timer_get_time();
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;

    pngle_t *png = lgfx_pngle_new();
    if (!png) {
        std::fclose(file);
        return false;
    }

    DecodeContext context;
    context.file = file;
    if (lgfx_pngle_prepare(png, png_read, &context) < 0) {
        ESP_LOGW(TAG, "PNG header decode failed path=%s", path);
        lgfx_pngle_destroy(png);
        std::fclose(file);
        return false;
    }

    const uint32_t source_width = lgfx_pngle_get_width(png);
    const uint32_t source_height = lgfx_pngle_get_height(png);
    uint8_t integer_scale = 0;
    if (source_width == output_width && source_height == output_height) integer_scale = 1;
    else if (source_width * 2 == output_width && source_height * 2 == output_height) integer_scale = 2;
    if (integer_scale == 0 || source_width > UINT16_MAX || source_height > UINT16_MAX) {
        ESP_LOGW(TAG, "PNG dimensions unsupported src=%ux%u output=%ux%u",
                 static_cast<unsigned>(source_width), static_cast<unsigned>(source_height),
                 static_cast<unsigned>(output_width), static_cast<unsigned>(output_height));
        lgfx_pngle_destroy(png);
        std::fclose(file);
        return false;
    }

    const size_t stride = (size_t(output_width) + 1) >> 1;
    const size_t frame_bytes = stride * output_height;
    auto *next_frame = static_cast<uint8_t *>(
        heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!next_frame) {
        ESP_LOGW(TAG, "4bpp staging allocation failed bytes=%u",
                 static_cast<unsigned>(frame_bytes));
        lgfx_pngle_destroy(png);
        std::fclose(file);
        return false;
    }
    std::memset(next_frame, 0xFF, frame_bytes);

    static const std::array<uint8_t, 256> gray_lut = make_contrast_gray4_lut();
    context.frame = next_frame;
    context.source_width = static_cast<uint16_t>(source_width);
    context.source_height = static_cast<uint16_t>(source_height);
    context.output_width = output_width;
    context.output_height = output_height;
    context.output_stride = static_cast<uint16_t>(stride);
    context.integer_scale = integer_scale;
    context.gray_lut = gray_lut.data();

    const int decode_result = lgfx_pngle_decomp(png, png_draw);
    lgfx_pngle_destroy(png);
    std::fclose(file);
    if (decode_result < 0 || context.bounds_error ||
        context.pixels < size_t(source_width) * source_height) {
        ESP_LOGW(TAG,
                 "PNG processing failed result=%d bounds=%d pixels=%u/%u",
                 decode_result, context.bounds_error ? 1 : 0,
                 static_cast<unsigned>(context.pixels),
                 static_cast<unsigned>(source_width * source_height));
        heap_caps_free(next_frame);
        return false;
    }

    reset();
    _data = next_frame;
    _width = output_width;
    _height = output_height;
    _nearest_scale = integer_scale;
    ESP_LOGI(TAG,
             "PNG contrast+gray4 ready src=%ux%u dst=%ux%u nearest=%ux "
             "black=%u white=%u bytes=%u time=%lld ms",
             static_cast<unsigned>(source_width), static_cast<unsigned>(source_height),
             static_cast<unsigned>(output_width), static_cast<unsigned>(output_height),
             static_cast<unsigned>(integer_scale),
             static_cast<unsigned>(CONTRAST_BLACK_POINT),
             static_cast<unsigned>(CONTRAST_WHITE_POINT),
             static_cast<unsigned>(frame_bytes),
             (esp_timer_get_time() - started) / 1000);
    return true;
}
