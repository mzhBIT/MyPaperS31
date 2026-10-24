#include "renderer/P4JpegDecoder.h"

#include <M5GFX.h>

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace p4_jpeg {
namespace {

static const char *TAG = "P4Jpeg";
static constexpr size_t MAX_COMPRESSED_BYTES = 12 * 1024 * 1024;
static constexpr size_t MAX_DECODED_BYTES = 12 * 1024 * 1024;
#if defined(CONFIG_IDF_TARGET_ESP32S31)
// The S31 EPD pipeline reserves about 4.7 MiB of PSRAM at boot, so the P4
// 8 MiB headroom would reject every decode. The buffers this path allocates
// are already accounted for separately, so keep a smaller safety margin.
static constexpr size_t MIN_FREE_PSRAM_RESERVE = 1024 * 1024;
#else
static constexpr size_t MIN_FREE_PSRAM_RESERVE = 8 * 1024 * 1024;
#endif
static constexpr float PPA_SCALE_STEP = 1.0f / 16.0f;
static jpeg_decoder_handle_t s_decoder = nullptr;

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

static bool checked_rgb565_size(uint32_t width, uint32_t height, size_t *size)
{
    if (!size || width == 0 || height == 0) return false;
    if (width > std::numeric_limits<size_t>::max() / height) return false;
    size_t pixels = (size_t)width * height;
    if (pixels > std::numeric_limits<size_t>::max() / 2) return false;
    *size = pixels * 2;
    return *size <= MAX_DECODED_BYTES;
}

static bool padded_dimensions(const jpeg_decode_picture_info_t &info, uint32_t *width, uint32_t *height)
{
    if (!width || !height || info.width == 0 || info.height == 0) return false;
    uint32_t mcu_w = 0;
    uint32_t mcu_h = 0;
    switch (info.sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444:
        mcu_w = 8;
        mcu_h = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        mcu_w = 16;
        mcu_h = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV420:
        mcu_w = 16;
        mcu_h = 16;
        break;
    default:
        // IDF 5.3 cannot convert a grayscale JPEG to RGB565. Let the
        // existing software path handle it.
        return false;
    }
    *width = align_up(info.width, mcu_w);
    *height = align_up(info.height, mcu_h);
    return true;
}

static uint8_t *alloc_jpeg_buffer(size_t requested,
                                  jpeg_dec_buffer_alloc_direction_t direction,
                                  size_t *allocated)
{
    jpeg_decode_memory_alloc_cfg_t cfg = {};
    cfg.buffer_direction = direction;
    return static_cast<uint8_t *>(jpeg_alloc_decoder_mem(requested, &cfg, allocated));
}

} // namespace

bool prepare()
{
    if (s_decoder) return true;
    jpeg_decode_engine_cfg_t engine_cfg = {};
    engine_cfg.timeout_ms = 1000;
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const esp_err_t err = jpeg_new_decoder_engine(&engine_cfg, &s_decoder);
    if (err != ESP_OK) {
        s_decoder = nullptr;
        ESP_LOGW(TAG,
                 "decoder reserve failed err=%s internal_free=%u largest=%u",
                 esp_err_to_name(err), (unsigned)internal_free, (unsigned)internal_largest);
        return false;
    }
    ESP_LOGI(TAG,
             "decoder engine reserved internal_free=%u largest=%u",
             (unsigned)internal_free, (unsigned)internal_largest);
    return true;
}

bool draw_file(m5gfx::M5GFX &gfx,
               const char *path,
               int x,
               int y,
               int box_width,
               int box_height,
               DrawStats *stats)
{
    DrawStats local_stats;
    local_stats.attempted = true;
    const int64_t total_start = esp_timer_get_time();
    auto finish = [&](bool ok) {
        local_stats.total_ms = (esp_timer_get_time() - total_start) / 1000;
        if (stats) *stats = local_stats;
        return ok;
    };

    if (!path || !path[0] || box_width <= 0 || box_height <= 0) return finish(false);
    if (!prepare()) return finish(false);

    struct stat st = {};
    if (stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > MAX_COMPRESSED_BYTES) {
        return finish(false);
    }
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if ((size_t)st.st_size > free_psram || free_psram - (size_t)st.st_size < MIN_FREE_PSRAM_RESERVE) {
        ESP_LOGW(TAG,
                 "insufficient PSRAM for input path=%s input=%u free=%u reserve=%u",
                 path,
                 (unsigned)st.st_size,
                 (unsigned)free_psram,
                 (unsigned)MIN_FREE_PSRAM_RESERVE);
        return finish(false);
    }

    size_t input_capacity = 0;
    uint8_t *input = alloc_jpeg_buffer((size_t)st.st_size, JPEG_DEC_ALLOC_INPUT_BUFFER, &input_capacity);
    if (!input || input_capacity < (size_t)st.st_size) {
        free(input);
        return finish(false);
    }

    int64_t started = esp_timer_get_time();
    FILE *fp = fopen(path, "rb");
    bool read_ok = fp && fread(input, 1, (size_t)st.st_size, fp) == (size_t)st.st_size;
    if (fp) fclose(fp);
    local_stats.read_ms = (esp_timer_get_time() - started) / 1000;
    if (!read_ok) {
        free(input);
        return finish(false);
    }

    jpeg_decode_picture_info_t info = {};
    started = esp_timer_get_time();
    esp_err_t err = jpeg_decoder_get_info(input, (uint32_t)st.st_size, &info);
    local_stats.header_ms = (esp_timer_get_time() - started) / 1000;
    local_stats.source_width = (int)info.width;
    local_stats.source_height = (int)info.height;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "header parse failed path=%s err=%s", path, esp_err_to_name(err));
        free(input);
        return finish(false);
    }

    uint32_t padded_w = 0;
    uint32_t padded_h = 0;
    size_t decoded_requested = 0;
    if (!padded_dimensions(info, &padded_w, &padded_h) ||
        !checked_rgb565_size(padded_w, padded_h, &decoded_requested)) {
        ESP_LOGW(TAG, "unsupported or oversized jpeg path=%s src=%lux%lu sample=%d",
                 path,
                 (unsigned long)info.width,
                 (unsigned long)info.height,
                 (int)info.sample_method);
        free(input);
        return finish(false);
    }

    size_t scaled_requested = 0;
    if (!checked_rgb565_size((uint32_t)box_width, (uint32_t)box_height, &scaled_requested)) {
        free(input);
        return finish(false);
    }
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (decoded_requested > free_psram ||
        scaled_requested > free_psram - decoded_requested ||
        free_psram - decoded_requested - scaled_requested < MIN_FREE_PSRAM_RESERVE) {
        ESP_LOGW(TAG,
                 "hardware working set too large path=%s decode=%u scale=%u free=%u reserve=%u",
                 path,
                 (unsigned)decoded_requested,
                 (unsigned)scaled_requested,
                 (unsigned)free_psram,
                 (unsigned)MIN_FREE_PSRAM_RESERVE);
        free(input);
        return finish(false);
    }

    size_t decoded_capacity = 0;
    uint8_t *decoded = alloc_jpeg_buffer(decoded_requested, JPEG_DEC_ALLOC_OUTPUT_BUFFER, &decoded_capacity);
    if (!decoded) {
        ESP_LOGW(TAG, "decode buffer alloc failed bytes=%u", (unsigned)decoded_requested);
        free(input);
        return finish(false);
    }

    jpeg_decode_cfg_t decode_cfg = {};
    decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // The default little-endian RGB565 byte layout matches lgfx::rgb565_t.
    decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t decoded_size = 0;
    started = esp_timer_get_time();
    err = jpeg_decoder_process(s_decoder,
                               &decode_cfg,
                               input,
                               (uint32_t)st.st_size,
                               decoded,
                               (uint32_t)decoded_capacity,
                               &decoded_size);
    local_stats.decode_ms = (esp_timer_get_time() - started) / 1000;
    free(input);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hardware decode failed path=%s err=%s", path, esp_err_to_name(err));
        free(decoded);
        return finish(false);
    }
    if (decoded_size != decoded_requested) {
        ESP_LOGW(TAG,
                 "unexpected decode layout path=%s bytes=%lu expected=%u",
                 path,
                 (unsigned long)decoded_size,
                 (unsigned)decoded_requested);
        free(decoded);
        return finish(false);
    }
    local_stats.decoded = true;

    float requested_scale = std::min((float)box_width / (float)info.width,
                                     (float)box_height / (float)info.height);
    float ppa_scale = std::floor(requested_scale / PPA_SCALE_STEP + 0.0001f) * PPA_SCALE_STEP;
    if (ppa_scale < PPA_SCALE_STEP || ppa_scale >= 16.0f) {
        ESP_LOGW(TAG, "PPA scale unsupported path=%s scale=%.4f", path, (double)requested_scale);
        free(decoded);
        return finish(false);
    }

    uint32_t output_w = std::max<uint32_t>(1, (uint32_t)(info.width * ppa_scale));
    uint32_t output_h = std::max<uint32_t>(1, (uint32_t)(info.height * ppa_scale));
    output_w = std::min<uint32_t>(output_w, (uint32_t)box_width);
    output_h = std::min<uint32_t>(output_h, (uint32_t)box_height);
    local_stats.output_width = (int)output_w;
    local_stats.output_height = (int)output_h;

    uint8_t *pixels = decoded;
    size_t output_capacity = decoded_capacity;
    bool need_ppa = ppa_scale != 1.0f || padded_w != info.width || padded_h != info.height;
    if (need_ppa) {
        size_t output_requested = 0;
        if (!checked_rgb565_size(output_w, output_h, &output_requested)) {
            free(decoded);
            return finish(false);
        }

        uint8_t *scaled = alloc_jpeg_buffer(output_requested, JPEG_DEC_ALLOC_OUTPUT_BUFFER, &output_capacity);
        if (!scaled) {
            ESP_LOGW(TAG, "scale buffer alloc failed bytes=%u", (unsigned)output_requested);
            free(decoded);
            return finish(false);
        }

        ppa_client_handle_t ppa = nullptr;
        ppa_client_config_t client_cfg = {};
        client_cfg.oper_type = PPA_OPERATION_SRM;
        client_cfg.max_pending_trans_num = 1;
        err = ppa_register_client(&client_cfg, &ppa);
        if (err == ESP_OK) {
            ppa_srm_oper_config_t cfg = {};
            cfg.in.buffer = decoded;
            cfg.in.pic_w = padded_w;
            cfg.in.pic_h = padded_h;
            cfg.in.block_w = info.width;
            cfg.in.block_h = info.height;
            cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            cfg.out.buffer = scaled;
            cfg.out.buffer_size = (uint32_t)output_capacity;
            cfg.out.pic_w = output_w;
            cfg.out.pic_h = output_h;
            cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
            cfg.scale_x = ppa_scale;
            cfg.scale_y = ppa_scale;
            cfg.mode = PPA_TRANS_MODE_BLOCKING;
            started = esp_timer_get_time();
            err = ppa_do_scale_rotate_mirror(ppa, &cfg);
            local_stats.scale_ms = (esp_timer_get_time() - started) / 1000;
            ppa_unregister_client(ppa);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PPA scale failed path=%s err=%s", path, esp_err_to_name(err));
            free(scaled);
            free(decoded);
            return finish(false);
        }
        local_stats.scaled = true;
        pixels = scaled;
    }

    int draw_x = x + std::max(0, (box_width - (int)output_w) / 2);
    int draw_y = y + std::max(0, (box_height - (int)output_h) / 2);
    started = esp_timer_get_time();
    gfx.pushImage(draw_x,
                  draw_y,
                  (int32_t)output_w,
                  (int32_t)output_h,
                  reinterpret_cast<const lgfx::rgb565_t *>(pixels));
    local_stats.push_ms = (esp_timer_get_time() - started) / 1000;

    if (pixels != decoded) free(pixels);
    free(decoded);
    return finish(true);
}

bool draw_memory(m5gfx::M5GFX &gfx,
                 const uint8_t *data,
                 size_t size,
                 const char *label,
                 int x,
                 int y,
                 int box_width,
                 int box_height,
                 DrawStats *stats)
{
    DrawStats local_stats;
    local_stats.attempted = true;
    const int64_t total_start = esp_timer_get_time();
    const char *source_label = label && label[0] ? label : "memory-jpeg";
    auto finish = [&](bool ok) {
        local_stats.total_ms = (esp_timer_get_time() - total_start) / 1000;
        if (stats) *stats = local_stats;
        return ok;
    };

    if (!data || size == 0 || size > MAX_COMPRESSED_BYTES ||
        box_width <= 0 || box_height <= 0) return finish(false);
    if (!prepare()) return finish(false);

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (size > free_psram || free_psram - size < MIN_FREE_PSRAM_RESERVE) {
        ESP_LOGW(TAG, "insufficient PSRAM for memory input label=%s input=%u free=%u",
                 source_label, (unsigned)size, (unsigned)free_psram);
        return finish(false);
    }

    size_t input_capacity = 0;
    uint8_t *input = alloc_jpeg_buffer(size, JPEG_DEC_ALLOC_INPUT_BUFFER, &input_capacity);
    if (!input || input_capacity < size) {
        free(input);
        return finish(false);
    }
    std::memcpy(input, data, size);

    jpeg_decode_picture_info_t info = {};
    int64_t started = esp_timer_get_time();
    esp_err_t err = jpeg_decoder_get_info(input, (uint32_t)size, &info);
    local_stats.header_ms = (esp_timer_get_time() - started) / 1000;
    local_stats.source_width = (int)info.width;
    local_stats.source_height = (int)info.height;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "header parse failed label=%s err=%s", source_label, esp_err_to_name(err));
        free(input);
        return finish(false);
    }

    uint32_t padded_w = 0;
    uint32_t padded_h = 0;
    size_t decoded_requested = 0;
    if (!padded_dimensions(info, &padded_w, &padded_h) ||
        !checked_rgb565_size(padded_w, padded_h, &decoded_requested)) {
        ESP_LOGW(TAG, "unsupported or oversized jpeg label=%s src=%lux%lu sample=%d",
                 source_label, (unsigned long)info.width, (unsigned long)info.height,
                 (int)info.sample_method);
        free(input);
        return finish(false);
    }

    size_t scaled_requested = 0;
    if (!checked_rgb565_size((uint32_t)box_width, (uint32_t)box_height, &scaled_requested)) {
        free(input);
        return finish(false);
    }
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (decoded_requested > free_psram ||
        scaled_requested > free_psram - decoded_requested ||
        free_psram - decoded_requested - scaled_requested < MIN_FREE_PSRAM_RESERVE) {
        ESP_LOGW(TAG, "hardware working set too large label=%s decode=%u scale=%u free=%u",
                 source_label, (unsigned)decoded_requested, (unsigned)scaled_requested,
                 (unsigned)free_psram);
        free(input);
        return finish(false);
    }

    size_t decoded_capacity = 0;
    uint8_t *decoded = alloc_jpeg_buffer(decoded_requested,
                                         JPEG_DEC_ALLOC_OUTPUT_BUFFER,
                                         &decoded_capacity);
    if (!decoded) {
        free(input);
        return finish(false);
    }

    jpeg_decode_cfg_t decode_cfg = {};
    decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t decoded_size = 0;
    started = esp_timer_get_time();
    err = jpeg_decoder_process(s_decoder, &decode_cfg, input, (uint32_t)size,
                               decoded, (uint32_t)decoded_capacity, &decoded_size);
    local_stats.decode_ms = (esp_timer_get_time() - started) / 1000;
    free(input);
    if (err != ESP_OK || decoded_size != decoded_requested) {
        ESP_LOGW(TAG, "hardware decode failed label=%s err=%s bytes=%lu expected=%u",
                 source_label, esp_err_to_name(err), (unsigned long)decoded_size,
                 (unsigned)decoded_requested);
        free(decoded);
        return finish(false);
    }
    local_stats.decoded = true;

    const float requested_scale = std::min((float)box_width / (float)info.width,
                                           (float)box_height / (float)info.height);
    const float ppa_scale = std::floor(requested_scale / PPA_SCALE_STEP + 0.0001f) *
                            PPA_SCALE_STEP;
    if (ppa_scale < PPA_SCALE_STEP || ppa_scale >= 16.0f) {
        ESP_LOGW(TAG, "PPA scale unsupported label=%s scale=%.4f",
                 source_label, (double)requested_scale);
        free(decoded);
        return finish(false);
    }

    uint32_t output_w = std::max<uint32_t>(1, (uint32_t)(info.width * ppa_scale));
    uint32_t output_h = std::max<uint32_t>(1, (uint32_t)(info.height * ppa_scale));
    output_w = std::min<uint32_t>(output_w, (uint32_t)box_width);
    output_h = std::min<uint32_t>(output_h, (uint32_t)box_height);
    local_stats.output_width = (int)output_w;
    local_stats.output_height = (int)output_h;

    uint8_t *pixels = decoded;
    size_t output_capacity = decoded_capacity;
    const bool need_ppa = ppa_scale != 1.0f || padded_w != info.width || padded_h != info.height;
    if (need_ppa) {
        size_t output_requested = 0;
        if (!checked_rgb565_size(output_w, output_h, &output_requested)) {
            free(decoded);
            return finish(false);
        }
        uint8_t *scaled = alloc_jpeg_buffer(output_requested,
                                            JPEG_DEC_ALLOC_OUTPUT_BUFFER,
                                            &output_capacity);
        if (!scaled) {
            free(decoded);
            return finish(false);
        }

        ppa_client_handle_t ppa = nullptr;
        ppa_client_config_t client_cfg = {};
        client_cfg.oper_type = PPA_OPERATION_SRM;
        client_cfg.max_pending_trans_num = 1;
        err = ppa_register_client(&client_cfg, &ppa);
        if (err == ESP_OK) {
            ppa_srm_oper_config_t cfg = {};
            cfg.in.buffer = decoded;
            cfg.in.pic_w = padded_w;
            cfg.in.pic_h = padded_h;
            cfg.in.block_w = info.width;
            cfg.in.block_h = info.height;
            cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            cfg.out.buffer = scaled;
            cfg.out.buffer_size = (uint32_t)output_capacity;
            cfg.out.pic_w = output_w;
            cfg.out.pic_h = output_h;
            cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
            cfg.scale_x = ppa_scale;
            cfg.scale_y = ppa_scale;
            cfg.mode = PPA_TRANS_MODE_BLOCKING;
            started = esp_timer_get_time();
            err = ppa_do_scale_rotate_mirror(ppa, &cfg);
            local_stats.scale_ms = (esp_timer_get_time() - started) / 1000;
            ppa_unregister_client(ppa);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PPA scale failed label=%s err=%s", source_label, esp_err_to_name(err));
            free(scaled);
            free(decoded);
            return finish(false);
        }
        local_stats.scaled = true;
        pixels = scaled;
    }

    const int draw_x = x + std::max(0, (box_width - (int)output_w) / 2);
    const int draw_y = y + std::max(0, (box_height - (int)output_h) / 2);
    started = esp_timer_get_time();
    gfx.pushImage(draw_x, draw_y, (int32_t)output_w, (int32_t)output_h,
                  reinterpret_cast<const lgfx::rgb565_t *>(pixels));
    local_stats.push_ms = (esp_timer_get_time() - started) / 1000;

    if (pixels != decoded) free(pixels);
    free(decoded);
    return finish(true);
}

} // namespace p4_jpeg
