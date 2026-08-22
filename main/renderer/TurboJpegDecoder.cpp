#include "renderer/TurboJpegDecoder.h"

#include <M5GFX.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

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
static constexpr size_t MIN_FREE_PSRAM_RESERVE = 8 * 1024 * 1024;
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

static uint16_t gray_to_rgb565(uint8_t gray)
{
    return (uint16_t)(((uint16_t)(gray & 0xF8) << 8) |
                      ((uint16_t)(gray & 0xFC) << 3) |
                      ((uint16_t)gray >> 3));
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

} // namespace

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

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram <= MIN_FREE_PSRAM_RESERVE + MIN_JPEG_MEMORY) {
        ESP_LOGW(TAG, "insufficient PSRAM path=%s free=%u reserve=%u",
                 path, (unsigned)free_psram, (unsigned)MIN_FREE_PSRAM_RESERVE);
        return finish(false);
    }
    size_t jpeg_budget = std::min(MAX_JPEG_MEMORY, free_psram - MIN_FREE_PSRAM_RESERVE);
    local_stats.memory_budget = (uint32_t)jpeg_budget;

    FILE *fp = fopen(path, "rb");
    if (!fp) return finish(false);

    jpeg_decompress_struct cinfo = {};
    ErrorManager error = {};
    uint8_t *source_row = nullptr;
    uint16_t *display_row = nullptr;
    bool created = false;
    bool ok = false;
    int64_t push_us = 0;

    cinfo.err = jpeg_std_error(&error.base);
    error.base.error_exit = error_exit;
    if (setjmp(error.jump)) {
        ESP_LOGW(TAG, "decode failed path=%s error=%s", path,
                 error.message[0] ? error.message : "unknown libjpeg error");
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    created = true;
    cinfo.mem->max_memory_to_use = (long)jpeg_budget;
    jpeg_stdio_src(&cinfo, fp);

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
    local_stats.scale_denom = choose_scale_denom(&cinfo, box_width, box_height);
    cinfo.scale_num = 1;
    cinfo.scale_denom = (unsigned)local_stats.scale_denom;

    {
        const int64_t decode_start = esp_timer_get_time();
        jpeg_start_decompress(&cinfo);
        local_stats.decoded_width = (int)cinfo.output_width;
        local_stats.decoded_height = (int)cinfo.output_height;

        if (cinfo.output_components != 1 || cinfo.output_width == 0 || cinfo.output_height == 0) {
            ESP_LOGW(TAG, "unexpected output path=%s size=%ux%u components=%d",
                     path, (unsigned)cinfo.output_width, (unsigned)cinfo.output_height,
                     cinfo.output_components);
            goto cleanup;
        }

        source_row = static_cast<uint8_t *>(heap_caps_malloc(cinfo.output_width, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        display_row = static_cast<uint16_t *>(heap_caps_malloc((size_t)box_width * sizeof(uint16_t),
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!source_row || !display_row) {
            ESP_LOGW(TAG, "scanline allocation failed path=%s src_w=%u dst_w=%d",
                     path, (unsigned)cinfo.output_width, box_width);
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
                    display_row[output_x] = gray_to_rgb565(source_row[source_x]);
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
    fclose(fp);
    return finish(ok);
}

} // namespace turbo_jpeg
