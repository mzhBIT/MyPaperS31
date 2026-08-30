#include "renderer/CoverThumbnail.h"

#include "renderer/TurboJpegDecoder.h"

#include <M5GFX.h>
#include "driver/jpeg_encode.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace cover_thumbnail {
namespace {

static const char *TAG = "COVER_THUMB";
static constexpr int THUMB_WIDTH = 240;
static constexpr int THUMB_HEIGHT = 336;
static constexpr int SQUARE_THUMB_SIZE = 360;
static constexpr int THUMB_QUALITY = 80;
static constexpr const char *THUMB_NAME = "cover_thumb_v1.jpg";
static constexpr const char *SQUARE_THUMB_NAME = "cover_square_v1.jpg";

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static std::string lower_ext(const std::string &path)
{
    size_t pos = path.find_last_of('.');
    std::string ext = pos == std::string::npos ? std::string() : path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    if (ext == ".jpeg") ext = ".jpg";
    return ext;
}

static bool valid_jpeg_file(const std::string &path)
{
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || st.st_size < 128 || S_ISDIR(st.st_mode)) return false;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    int first = std::fgetc(fp);
    int second = std::fgetc(fp);
    std::fclose(fp);
    return first == 0xff && second == 0xd8;
}

static bool probe_png(FILE *fp, int *width, int *height)
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

static bool probe_bmp(FILE *fp, int *width, int *height)
{
    uint8_t header[26];
    if (std::fseek(fp, 0, SEEK_SET) != 0 ||
        std::fread(header, 1, sizeof(header), fp) != sizeof(header)) return false;
    if (header[0] != 'B' || header[1] != 'M') return false;
    int32_t bmp_width = (int32_t)le32(header + 18);
    int32_t bmp_height = (int32_t)le32(header + 22);
    if (bmp_width == 0 || bmp_height == 0) return false;
    *width = bmp_width < 0 ? -bmp_width : bmp_width;
    *height = bmp_height < 0 ? -bmp_height : bmp_height;
    return true;
}

static bool probe_jpeg(FILE *fp, int *width, int *height, bool *progressive)
{
    *progressive = false;
    if (std::fseek(fp, 0, SEEK_SET) != 0 || std::fgetc(fp) != 0xff || std::fgetc(fp) != 0xd8) return false;
    while (true) {
        int marker = std::fgetc(fp);
        while (marker == 0xff) marker = std::fgetc(fp);
        if (marker == EOF || marker == 0xd9 || marker == 0xda) return false;
        uint8_t length_bytes[2];
        if (std::fread(length_bytes, 1, 2, fp) != 2) return false;
        int length = be16(length_bytes);
        if (length < 2) return false;
        bool sof = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                   (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
        if (sof) {
            uint8_t dimensions[5];
            if (std::fread(dimensions, 1, sizeof(dimensions), fp) != sizeof(dimensions)) return false;
            *height = be16(dimensions + 1);
            *width = be16(dimensions + 3);
            *progressive = marker == 0xc2 || marker == 0xc6 || marker == 0xca || marker == 0xce;
            return *width > 0 && *height > 0;
        }
        if (std::fseek(fp, length - 2, SEEK_CUR) != 0) return false;
    }
}

static bool probe_image(const std::string &path, const std::string &ext,
                        int *width, int *height, bool *progressive)
{
    *width = 0;
    *height = 0;
    *progressive = false;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = false;
    if (ext == ".jpg") ok = probe_jpeg(fp, width, height, progressive);
    else if (ext == ".png") ok = probe_png(fp, width, height);
    else if (ext == ".bmp") ok = probe_bmp(fp, width, height);
    std::fclose(fp);
    return ok;
}

static bool render_source(lgfx::LGFX_Sprite *sprite, const std::string &path,
                          const std::string &ext, int source_width, int source_height,
                          bool progressive, int target_width, int target_height)
{
    float scale = std::min((float)target_width / (float)source_width,
                           (float)target_height / (float)source_height);
    int draw_width = std::max(1, std::min(target_width, (int)std::lround(source_width * scale)));
    int draw_height = std::max(1, std::min(target_height, (int)std::lround(source_height * scale)));
    int draw_x = (target_width - draw_width) / 2;
    int draw_y = (target_height - draw_height) / 2;

    if (ext == ".jpg") {
        if (progressive) {
            return turbo_jpeg::draw_file(*sprite, path.c_str(), draw_x, draw_y, draw_width, draw_height);
        }
        if (sprite->drawJpgFile(path.c_str(), draw_x, draw_y, draw_width, draw_height,
                                0, 0, scale, scale)) return true;
        return turbo_jpeg::draw_file(*sprite, path.c_str(), draw_x, draw_y, draw_width, draw_height);
    }
    if (ext == ".png") {
        return sprite->drawPngFile(path.c_str(), draw_x, draw_y, draw_width, draw_height,
                                   0, 0, scale, scale);
    }
    if (ext == ".bmp") {
        return sprite->drawBmpFile(path.c_str(), draw_x, draw_y, draw_width, draw_height,
                                   0, 0, scale, scale);
    }
    return false;
}

static bool encode_hardware_jpeg(lgfx::LGFX_Sprite *sprite, const std::string &output_path,
                                 int width, int height)
{
    const size_t raw_size = (size_t)width * height * 3;
    jpeg_encode_memory_alloc_cfg_t input_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    jpeg_encode_memory_alloc_cfg_t output_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t input_capacity = 0;
    size_t output_capacity = 0;
    uint8_t *input = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(raw_size, &input_cfg, &input_capacity));
    uint8_t *output = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(raw_size, &output_cfg, &output_capacity));
    if (!input || !output || input_capacity < raw_size) {
        std::free(output);
        std::free(input);
        return false;
    }
    sprite->readRectRGB(0, 0, width, height, input);

    jpeg_encoder_handle_t encoder = nullptr;
    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    esp_err_t err = jpeg_new_encoder_engine(&engine_cfg, &encoder);
    uint32_t encoded_size = 0;
    if (err == ESP_OK) {
        jpeg_encode_cfg_t encode_cfg = {
            .height = static_cast<uint32_t>(height),
            .width = static_cast<uint32_t>(width),
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = THUMB_QUALITY,
        };
        err = jpeg_encoder_process(encoder, &encode_cfg, input, (uint32_t)raw_size,
                                   output, (uint32_t)output_capacity, &encoded_size);
        jpeg_del_encoder_engine(encoder);
    }
    std::free(input);
    if (err != ESP_OK || encoded_size < 128 || encoded_size > output_capacity) {
        ESP_LOGW(TAG, "hardware encode failed err=%s bytes=%lu", esp_err_to_name(err),
                 (unsigned long)encoded_size);
        std::free(output);
        return false;
    }

    std::string temp_path = output_path + ".tmp";
    FILE *fp = std::fopen(temp_path.c_str(), "wb");
    bool ok = fp && std::fwrite(output, 1, encoded_size, fp) == encoded_size;
    if (fp && std::fclose(fp) != 0) ok = false;
    std::free(output);
    if (!ok) {
        if (fp == nullptr) ESP_LOGW(TAG, "open thumbnail failed path=%s", temp_path.c_str());
        std::remove(temp_path.c_str());
        return false;
    }
    std::remove(output_path.c_str());
    if (std::rename(temp_path.c_str(), output_path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace

std::string cache_path_for(const std::string &source_path)
{
    size_t slash = source_path.find_last_of('/');
    if (slash == std::string::npos) return THUMB_NAME;
    return source_path.substr(0, slash + 1) + THUMB_NAME;
}

std::string square_cache_path_for(const std::string &source_path)
{
    size_t slash = source_path.find_last_of('/');
    if (slash == std::string::npos) return SQUARE_THUMB_NAME;
    return source_path.substr(0, slash + 1) + SQUARE_THUMB_NAME;
}

static bool ensure_sized(const std::string &source_path, const std::string &thumbnail_path,
                         int width, int height)
{
    if (valid_jpeg_file(thumbnail_path)) return true;
    std::string ext = lower_ext(source_path);
    int source_width = 0;
    int source_height = 0;
    bool progressive = false;
    if (!probe_image(source_path, ext, &source_width, &source_height, &progressive)) return false;

    const int64_t started = esp_timer_get_time();
    lgfx::LGFX_Sprite sprite;
    sprite.setPsram(true);
    sprite.setColorDepth(16);
    if (!sprite.createSprite(width, height)) {
        ESP_LOGW(TAG, "sprite allocation failed");
        return false;
    }
    sprite.fillScreen(TFT_WHITE);
    if (!render_source(&sprite, source_path, ext, source_width, source_height,
                       progressive, width, height)) {
        ESP_LOGW(TAG, "cover decode failed path=%s", source_path.c_str());
        return false;
    }
    if (!encode_hardware_jpeg(&sprite, thumbnail_path, width, height)) return false;

    struct stat st = {};
    stat(thumbnail_path.c_str(), &st);
    ESP_LOGI(TAG, "created %dx%d jpeg bytes=%ld elapsed=%lld ms source=%s",
             width, height, (long)st.st_size,
             (long long)((esp_timer_get_time() - started) / 1000), source_path.c_str());
    return true;
}

bool ensure(const std::string &source_path, const std::string &thumbnail_path)
{
    return ensure_sized(source_path, thumbnail_path, THUMB_WIDTH, THUMB_HEIGHT);
}

bool ensure_square(const std::string &source_path, const std::string &thumbnail_path)
{
    return ensure_sized(source_path, thumbnail_path, SQUARE_THUMB_SIZE, SQUARE_THUMB_SIZE);
}

} // namespace cover_thumbnail
