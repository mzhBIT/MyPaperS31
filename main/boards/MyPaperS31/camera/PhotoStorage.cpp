#include "PhotoStorage.h"
#include "storage/storage_sdcard.h"
#include "time/time_basis.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "jpeglib.h"
}

namespace mypaper::board::s31::camera {
namespace {
constexpr const char *TAG = "CAMERA_PHOTO";
constexpr const char *directory = STORAGE_SDCARD_MOUNT_POINT "/images/camera";
struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};
// All state modified after setjmp lives in this heap object, including the
// compressor. Fatal encoder errors therefore have a defined cleanup path.
struct Encoder {
    jpeg_compress_struct jpeg;
    JpegError error;
    FILE *file;
    uint8_t *row;
    esp_err_t result;
    bool owns_temporary;
    char path[photo_path_capacity];
    char temporary[photo_path_capacity + 8];
};
void jpeg_error(j_common_ptr common) {
    auto *error = reinterpret_cast<JpegError *>(common->err);
    common->err->format_message(common, error->message);
    longjmp(error->jump, 1);
}
bool ensure_directory(const char *path) {
    if (::mkdir(path, 0775) == 0) return true;
    struct stat info{};
    return errno == EEXIST && ::stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
esp_err_t open_photo(Encoder *encoder) {
    if (!storage_sdcard_get_card()) return ESP_ERR_NOT_FOUND;
    if (!ensure_directory(STORAGE_SDCARD_MOUNT_POINT "/images") || !ensure_directory(directory)) {
        ESP_LOGW(TAG, "photo directory unavailable: errno=%d", errno);
        return ESP_FAIL;
    }
    char prefix[48]{};
    time_t local = std::time(nullptr) + time_basis_get_offset_minutes()*60;
    struct tm calendar{};
    if (time_basis_system_clock_valid() && gmtime_r(&local, &calendar))
        std::strftime(prefix, sizeof(prefix), "IMG_%Y%m%d_%H%M%S", &calendar);
    else
        std::snprintf(prefix, sizeof(prefix), "IMG_UNTIMED_%08lX", (unsigned long)esp_random());
    for (unsigned suffix = 1; suffix <= 9999; ++suffix) {
        const int length = std::snprintf(encoder->path, sizeof(encoder->path),
            "%s/%s_%04u.jpg", directory, prefix, suffix);
        if (length < 0 || size_t(length) >= sizeof(encoder->path)) return ESP_ERR_INVALID_SIZE;
        struct stat info{};
        if (::stat(encoder->path, &info) == 0) continue;
        if (errno != ENOENT) return ESP_FAIL;
        std::snprintf(encoder->temporary, sizeof(encoder->temporary), "%s.tmp", encoder->path);
        const int fd = ::open(encoder->temporary, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            ESP_LOGW(TAG, "photo open failed: errno=%d", errno);
            return ESP_FAIL;
        }
        encoder->owns_temporary = true;
        encoder->file = ::fdopen(fd, "wb");
        if (!encoder->file) { ::close(fd); return ESP_FAIL; }
        return ESP_OK;
    }
    return ESP_FAIL;
}
}

esp_err_t save_rgb565_photo(const uint8_t *rgb, int width, int height,
    char *saved_path, size_t capacity, bool (*cancelled)(void*), void *context) {
    if (saved_path && capacity) saved_path[0] = '\0';
    if (!rgb || width <= 0 || width > 640 || height <= 0 || height > 480 ||
        !saved_path || capacity < photo_path_capacity) return ESP_ERR_INVALID_ARG;
    if (cancelled && cancelled(context)) return ESP_ERR_INVALID_STATE;
    const int64_t begin = esp_timer_get_time();
    auto *const encoder = static_cast<Encoder *>(heap_caps_calloc(1, sizeof(Encoder),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!encoder) return ESP_ERR_NO_MEM;
    encoder->row = static_cast<uint8_t *>(heap_caps_malloc(size_t(width)*3,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    encoder->result = encoder->row ? open_photo(encoder) : ESP_ERR_NO_MEM;
    if (encoder->result == ESP_OK) {
        encoder->jpeg.err = jpeg_std_error(&encoder->error.base);
        encoder->error.base.error_exit = jpeg_error;
        if (setjmp(encoder->error.jump)) {
            ESP_LOGW(TAG, "JPEG encode failed: %s", encoder->error.message);
            encoder->result = ESP_FAIL;
        } else {
            jpeg_create_compress(&encoder->jpeg);
            jpeg_stdio_dest(&encoder->jpeg, encoder->file);
            encoder->jpeg.image_width = width;
            encoder->jpeg.image_height = height;
            encoder->jpeg.input_components = 3;
            encoder->jpeg.in_color_space = JCS_RGB;
            jpeg_set_defaults(&encoder->jpeg);
            jpeg_set_quality(&encoder->jpeg, 90, TRUE);
            jpeg_start_compress(&encoder->jpeg, TRUE);
            while (encoder->jpeg.next_scanline < encoder->jpeg.image_height) {
                if (cancelled && cancelled(context)) { encoder->result = ESP_ERR_INVALID_STATE; break; }
                const auto *source = rgb + size_t(encoder->jpeg.next_scanline)*width*2;
                for (int x = 0; x < width; ++x) {
                    const unsigned pixel = (unsigned(source[2*x])<<8) | source[2*x+1];
                    encoder->row[3*x]   = uint8_t(((pixel>>11)&31)*255/31);
                    encoder->row[3*x+1] = uint8_t(((pixel>>5)&63)*255/63);
                    encoder->row[3*x+2] = uint8_t((pixel&31)*255/31);
                }
                JSAMPROW row = encoder->row;
                if (jpeg_write_scanlines(&encoder->jpeg, &row, 1) != 1) { encoder->result = ESP_FAIL; break; }
                // Keep lower-priority UI/idle tasks responsive even when the
                // JPEG encoder and FAT writes complete without blocking.
                if ((encoder->jpeg.next_scanline & 31u) == 0) vTaskDelay(1);
            }
            if (encoder->result == ESP_OK) jpeg_finish_compress(&encoder->jpeg);
        }
        if (encoder->jpeg.mem) jpeg_destroy_compress(&encoder->jpeg);
    }
    if (encoder->file) {
        if (encoder->result == ESP_OK &&
            (std::fflush(encoder->file) != 0 || ::fsync(::fileno(encoder->file)) != 0)) encoder->result = ESP_FAIL;
        if (std::fclose(encoder->file) != 0) encoder->result = ESP_FAIL;
    }
    if (encoder->result == ESP_OK && cancelled && cancelled(context)) encoder->result = ESP_ERR_INVALID_STATE;
    if (encoder->result == ESP_OK) {
        // A single camera worker owns this filename namespace. Neither an
        // existing photo nor a stale .tmp file is reused or overwritten.
        struct stat info{};
        if (::stat(encoder->path, &info) == 0 || errno != ENOENT ||
            std::rename(encoder->temporary, encoder->path) != 0) encoder->result = ESP_FAIL;
    }
    if (encoder->result == ESP_OK) std::snprintf(saved_path, capacity, "%s", encoder->path);
    else if (encoder->owns_temporary) std::remove(encoder->temporary);
    ESP_LOGI(TAG, "photo save: result=%s path=%s time=%lld ms", esp_err_to_name(encoder->result),
        encoder->path, (esp_timer_get_time()-begin)/1000);
    const auto result = encoder->result;
    heap_caps_free(encoder->row);
    heap_caps_free(encoder);
    return result;
}
}
