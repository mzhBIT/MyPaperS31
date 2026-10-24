#include "renderer/TurboJpegDecoder.h"

#include <M5GFX.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "jpeglib.h"
}

namespace turbo_jpeg {
namespace {

static const char *TAG = "TurboJpeg";
static constexpr size_t MAX_JPEG_MEMORY = 12 * 1024 * 1024;
#if defined(CONFIG_IDF_TARGET_ESP32S31)
// S31 has far less PSRAM than the P4 and the EPD 2D pipeline reserves about
// 4.7 MiB before the app starts, so the P4-sized 8 MiB headroom can never be
// satisfied. This decoder streams one scanline at a time, so a smaller reserve
// is enough to keep the rest of the UI safe.
static constexpr size_t MIN_FREE_PSRAM_RESERVE = 1024 * 1024;
#else
static constexpr size_t MIN_FREE_PSRAM_RESERVE = 8 * 1024 * 1024;
#endif
static constexpr size_t MIN_JPEG_MEMORY = 1024 * 1024;

struct ErrorManager {
    jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

static void error_exit(j_common_ptr common)
{
    ErrorManager *error = reinterpret_cast<ErrorManager *>(common->err);
    (*common->err->format_message)(common, error->message);
    longjmp(error->jump, 1);
}

static int choose_scale_denom(j_decompress_ptr cinfo, int target_width, int target_height)
{
    static const int denoms[] = {8, 4, 2, 1};
    for (int denom : denoms) {
        cinfo->scale_num = 1;
        cinfo->scale_denom = denom;
        jpeg_calc_output_dimensions(cinfo);
        if ((int)cinfo->output_width >= target_width &&
            (int)cinfo->output_height >= target_height) {
            return denom;
        }
    }
    return 1;
}

static uint32_t clamp_u32(size_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static size_t progressive_coefficient_bytes(const jpeg_decompress_struct *cinfo,
                                            int scale_denom,
                                            size_t *compact_bytes)
{
    if (compact_bytes) *compact_bytes = 0;
    if (!cinfo || !cinfo->progressive_mode) return 0;

    const bool grayscale_y_only = cinfo->out_color_space == JCS_GRAYSCALE &&
                                 (cinfo->jpeg_color_space == JCS_GRAYSCALE ||
                                  cinfo->jpeg_color_space == JCS_YCbCr);
    const int keep_size = std::max(1, DCTSIZE / std::max(1, scale_denom));
    size_t legacy_bytes = 0;
    size_t compact = 0;

    for (int ci = 0; ci < cinfo->num_components; ++ci) {
        const jpeg_component_info &component = cinfo->comp_info[ci];
        const size_t blocks_per_row = (component.width_in_blocks +
                                       component.h_samp_factor - 1) /
                                      component.h_samp_factor *
                                      component.h_samp_factor;
        const size_t block_rows = (component.height_in_blocks +
                                   component.v_samp_factor - 1) /
                                  component.v_samp_factor *
                                  component.v_samp_factor;
        const size_t block_count = blocks_per_row * block_rows;
        legacy_bytes += block_count * sizeof(JBLOCK);

        const bool needed = !grayscale_y_only || ci == 0;
        if (!needed) {
            compact += block_count * sizeof(uint64_t);
        } else if (keep_size < DCTSIZE) {
            compact += block_count *
                       (sizeof(uint64_t) +
                        (size_t)keep_size * keep_size * sizeof(JCOEF));
        } else {
            compact += block_count * sizeof(JBLOCK);
        }
    }

    if (compact_bytes) *compact_bytes = compact;
    return legacy_bytes;
}

// Progressive JPEGs must retain every DCT coefficient until the final scan, so
// the IDCT scale also determines peak memory. Start from the sharpest scale
// that still covers the target box and fall back to a coarser one (each step
// stores 4x fewer coefficients) instead of letting libjpeg abort with
// "Memory limit exceeded".
static int choose_scale_denom_for_budget(j_decompress_ptr cinfo, size_t budget,
                                         int target_width, int target_height)
{
    int denom = choose_scale_denom(cinfo, target_width, target_height);
    if (!cinfo->progressive_mode) return denom;

    // Row buffers and other libjpeg structures share the same budget.
    constexpr size_t COEFFICIENT_MARGIN = 512 * 1024;
    while (denom < 8) {
        cinfo->scale_num = 1;
        cinfo->scale_denom = (unsigned)denom;
        jpeg_calc_output_dimensions(cinfo);
        const size_t needed = progressive_coefficient_bytes(cinfo, denom, nullptr);
        if (needed == 0 || needed + COEFFICIENT_MARGIN <= budget) break;
        denom *= 2;
    }
    return denom;
}

static void log_progressive_estimate(j_decompress_ptr cinfo,
                                     const char *path,
                                     int scale_denom,
                                     DrawStats *stats)
{
    if (!cinfo || !stats || !cinfo->progressive_mode) return;

    size_t compact_bytes = 0;
    const size_t legacy_bytes = progressive_coefficient_bytes(cinfo, scale_denom,
                                                              &compact_bytes);
    stats->legacy_coefficient_bytes = clamp_u32(legacy_bytes);
    stats->compact_coefficient_bytes = clamp_u32(compact_bytes);
    ESP_LOGI(TAG,
             "progressive compact path=%s src=%ux%u denom=%d coefficients=%u->%u bytes saved=%u budget=%u",
             path ? path : "",
             (unsigned)cinfo->image_width,
             (unsigned)cinfo->image_height,
             scale_denom,
             (unsigned)stats->legacy_coefficient_bytes,
             (unsigned)stats->compact_coefficient_bytes,
             (unsigned)(stats->legacy_coefficient_bytes >= stats->compact_coefficient_bytes
                          ? stats->legacy_coefficient_bytes - stats->compact_coefficient_bytes
                          : 0),
             (unsigned)stats->memory_budget);
}

} // namespace

static bool draw_source(lgfx::LGFXBase &gfx,
                        const char *path,
                        const uint8_t *data,
                        size_t data_size,
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

    if ((!data && (!path || !path[0])) || (data && data_size == 0) ||
        box_width <= 0 || box_height <= 0) return finish(false);
    const char *source_label = path && path[0] ? path : "memory";

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram <= MIN_FREE_PSRAM_RESERVE + MIN_JPEG_MEMORY) {
        ESP_LOGW(TAG, "insufficient PSRAM path=%s free=%u reserve=%u",
                 source_label, (unsigned)free_psram, (unsigned)MIN_FREE_PSRAM_RESERVE);
        return finish(false);
    }
    size_t jpeg_budget = std::min(MAX_JPEG_MEMORY, free_psram - MIN_FREE_PSRAM_RESERVE);
    local_stats.memory_budget = (uint32_t)jpeg_budget;

    FILE *fp = data ? nullptr : fopen(path, "rb");
    if (!data && !fp) return finish(false);

    jpeg_decompress_struct cinfo = {};
    ErrorManager error = {};
    uint8_t *source_row = nullptr;
    lgfx::grayscale_t *display_row = nullptr;
    bool created = false;
    bool ok = false;
    int64_t push_us = 0;

    cinfo.err = jpeg_std_error(&error.base);
    error.base.error_exit = error_exit;
    if (setjmp(error.jump)) {
        ESP_LOGW(TAG, "decode failed path=%s error=%s", source_label,
                 error.message[0] ? error.message : "unknown libjpeg error");
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    created = true;
    cinfo.mem->max_memory_to_use = (long)jpeg_budget;
    if (data) jpeg_mem_src(&cinfo, data, data_size);
    else jpeg_stdio_src(&cinfo, fp);

    {
        const int64_t header_start = esp_timer_get_time();
        jpeg_read_header(&cinfo, TRUE);
        local_stats.header_ms = (esp_timer_get_time() - header_start) / 1000;
    }
    local_stats.source_width = (int)cinfo.image_width;
    local_stats.source_height = (int)cinfo.image_height;
    local_stats.progressive = cinfo.progressive_mode != 0;

    cinfo.out_color_space = JCS_GRAYSCALE;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    local_stats.scale_denom = choose_scale_denom_for_budget(&cinfo, jpeg_budget,
                                                            box_width, box_height);
    cinfo.scale_num = 1;
    cinfo.scale_denom = (unsigned)local_stats.scale_denom;
    log_progressive_estimate(&cinfo, source_label, local_stats.scale_denom,
                             &local_stats);

    {
        const int64_t decode_start = esp_timer_get_time();
        jpeg_start_decompress(&cinfo);
        local_stats.decoded_width = (int)cinfo.output_width;
        local_stats.decoded_height = (int)cinfo.output_height;

        if (cinfo.output_components != 1 || cinfo.output_width == 0 || cinfo.output_height == 0) {
            ESP_LOGW(TAG, "unexpected output path=%s size=%ux%u components=%d",
                     source_label, (unsigned)cinfo.output_width, (unsigned)cinfo.output_height,
                     cinfo.output_components);
            goto cleanup;
        }

        source_row = static_cast<uint8_t *>(heap_caps_malloc(cinfo.output_width, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        display_row = static_cast<lgfx::grayscale_t *>(heap_caps_malloc(
            (size_t)box_width * sizeof(lgfx::grayscale_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!source_row || !display_row) {
            ESP_LOGW(TAG, "scanline allocation failed path=%s src_w=%u dst_w=%d",
                     source_label, (unsigned)cinfo.output_width, box_width);
            goto cleanup;
        }

        int next_output_y = 0;
        while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW rows[1] = {source_row};
            if (jpeg_read_scanlines(&cinfo, rows, 1) != 1) goto cleanup;
            const int source_y = (int)cinfo.output_scanline - 1;

            while (next_output_y < box_height &&
                   (int)((int64_t)next_output_y * cinfo.output_height / box_height) == source_y) {
                for (int output_x = 0; output_x < box_width; ++output_x) {
                    size_t source_x = (size_t)((int64_t)output_x * cinfo.output_width / box_width);
                    if (source_x >= cinfo.output_width) source_x = cinfo.output_width - 1;
                    display_row[output_x] = source_row[source_x];
                }
                const int64_t push_start = esp_timer_get_time();
                gfx.pushImage(x, y + next_output_y, box_width, 1, display_row);
                push_us += esp_timer_get_time() - push_start;
                ++next_output_y;
            }
        }
        jpeg_finish_decompress(&cinfo);
        const int64_t decode_elapsed = esp_timer_get_time() - decode_start;
        local_stats.push_ms = push_us / 1000;
        local_stats.decode_ms = std::max<int64_t>(0, decode_elapsed - push_us) / 1000;
        ok = next_output_y == box_height;
    }

cleanup:
    free(display_row);
    free(source_row);
    if (created) jpeg_destroy_decompress(&cinfo);
    if (fp) fclose(fp);
    return finish(ok);
}

bool draw_file(lgfx::LGFXBase &gfx,
               const char *path,
               int x,
               int y,
               int box_width,
               int box_height,
               DrawStats *stats)
{
    return draw_source(gfx, path, nullptr, 0, x, y, box_width, box_height, stats);
}

bool draw_memory(lgfx::LGFXBase &gfx,
                 const uint8_t *data,
                 size_t size,
                 const char *label,
                 int x,
                 int y,
                 int box_width,
                 int box_height,
                 DrawStats *stats)
{
    return draw_source(gfx, label, data, size, x, y, box_width, box_height, stats);
}

bool decode_memory_gray4(const uint8_t *data,
                         size_t size,
                         const char *label,
                         int output_width,
                         int output_height,
                         std::vector<uint8_t> *out,
                         DrawStats *stats)
{
    DrawStats local_stats;
    local_stats.attempted = true;
    const int64_t total_start = esp_timer_get_time();
    auto finish = [&](bool ok) {
        local_stats.total_ms = (esp_timer_get_time() - total_start) / 1000;
        if (stats) *stats = local_stats;
        if (!ok && out) std::vector<uint8_t>().swap(*out);
        return ok;
    };

    if (!data || size == 0 || !out || output_width <= 0 || output_height <= 0) {
        return finish(false);
    }
    const char *source_label = label && label[0] ? label : "memory";
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram <= MIN_FREE_PSRAM_RESERVE + MIN_JPEG_MEMORY) {
        ESP_LOGW(TAG, "insufficient PSRAM path=%s free=%u reserve=%u",
                 source_label, (unsigned)free_psram, (unsigned)MIN_FREE_PSRAM_RESERVE);
        return finish(false);
    }
    const size_t jpeg_budget = std::min(MAX_JPEG_MEMORY,
                                        free_psram - MIN_FREE_PSRAM_RESERVE);
    local_stats.memory_budget = (uint32_t)jpeg_budget;

    jpeg_decompress_struct cinfo = {};
    ErrorManager error = {};
    uint8_t *source_row = nullptr;
    bool created = false;
    bool ok = false;

    cinfo.err = jpeg_std_error(&error.base);
    error.base.error_exit = error_exit;
    if (setjmp(error.jump)) {
        ESP_LOGW(TAG, "decode failed path=%s error=%s", source_label,
                 error.message[0] ? error.message : "unknown libjpeg error");
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    created = true;
    cinfo.mem->max_memory_to_use = (long)jpeg_budget;
    jpeg_mem_src(&cinfo, data, size);

    {
        const int64_t header_start = esp_timer_get_time();
        jpeg_read_header(&cinfo, TRUE);
        local_stats.header_ms = (esp_timer_get_time() - header_start) / 1000;
    }
    local_stats.source_width = (int)cinfo.image_width;
    local_stats.source_height = (int)cinfo.image_height;
    local_stats.progressive = cinfo.progressive_mode != 0;
    cinfo.out_color_space = JCS_GRAYSCALE;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    local_stats.scale_denom = choose_scale_denom_for_budget(&cinfo, jpeg_budget,
                                                            output_width, output_height);
    cinfo.scale_num = 1;
    cinfo.scale_denom = (unsigned)local_stats.scale_denom;
    log_progressive_estimate(&cinfo, source_label, local_stats.scale_denom,
                             &local_stats);

    {
        const int64_t decode_start = esp_timer_get_time();
        jpeg_start_decompress(&cinfo);
        local_stats.decoded_width = (int)cinfo.output_width;
        local_stats.decoded_height = (int)cinfo.output_height;
        if (cinfo.output_components != 1 || cinfo.output_width == 0 ||
            cinfo.output_height == 0) {
            ESP_LOGW(TAG, "unexpected output path=%s size=%ux%u components=%d",
                     source_label, (unsigned)cinfo.output_width,
                     (unsigned)cinfo.output_height, cinfo.output_components);
            goto cleanup;
        }

        source_row = static_cast<uint8_t *>(heap_caps_malloc(
            cinfo.output_width, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!source_row) {
            ESP_LOGW(TAG, "scanline allocation failed path=%s src_w=%u",
                     source_label, (unsigned)cinfo.output_width);
            goto cleanup;
        }

        const size_t stride = ((size_t)output_width + 1) >> 1;
        out->assign(stride * (size_t)output_height, 0xFF);
        if (out->empty()) goto cleanup;

        int next_output_y = 0;
        while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW rows[1] = {source_row};
            if (jpeg_read_scanlines(&cinfo, rows, 1) != 1) goto cleanup;
            const int source_y = (int)cinfo.output_scanline - 1;
            while (next_output_y < output_height &&
                   (int)((int64_t)next_output_y * cinfo.output_height /
                         output_height) == source_y) {
                uint8_t *dst = out->data() + (size_t)next_output_y * stride;
                for (int output_x = 0; output_x < output_width; output_x += 2) {
                    size_t source_x0 = (size_t)((int64_t)output_x *
                                                cinfo.output_width / output_width);
                    if (source_x0 >= cinfo.output_width) source_x0 = cinfo.output_width - 1;
                    const uint8_t gray0 = source_row[source_x0] >> 4;
                    uint8_t gray1 = 0x0F;
                    if (output_x + 1 < output_width) {
                        size_t source_x1 = (size_t)((int64_t)(output_x + 1) *
                                                    cinfo.output_width / output_width);
                        if (source_x1 >= cinfo.output_width) source_x1 = cinfo.output_width - 1;
                        gray1 = source_row[source_x1] >> 4;
                    }
                    dst[output_x >> 1] = (uint8_t)((gray0 << 4) | gray1);
                }
                ++next_output_y;
            }
        }
        jpeg_finish_decompress(&cinfo);
        local_stats.decode_ms = (esp_timer_get_time() - decode_start) / 1000;
        ok = next_output_y == output_height;
    }

cleanup:
    free(source_row);
    if (created) jpeg_destroy_decompress(&cinfo);
    return finish(ok);
}

} // namespace turbo_jpeg
