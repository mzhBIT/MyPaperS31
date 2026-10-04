#include "audio_radio.h"
#include "audio/mp3_duration.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "sdkconfig.h"
#include "audio/m5_i2c_codec_ctrl.h"
#include "audio/usb_audio_output.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_extractor.h"
#include "esp_extractor_defaults.h"
#include "esp_fourcc.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_hls_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "nvs.h"

#ifndef ES8311_CODEC_DEFAULT_ADDR
#define ES8311_CODEC_DEFAULT_ADDR 0x30
#endif

#ifndef CONFIG_AUDIO_RADIO_I2C_PORT
#if CONFIG_IDF_TARGET_ESP32P4
#define CONFIG_AUDIO_RADIO_I2C_PORT 1
#else
#define CONFIG_AUDIO_RADIO_I2C_PORT 0
#endif
#endif

#ifndef CONFIG_MYPAPER_USB_AUDIO_MAX_VOLUME
#define CONFIG_MYPAPER_USB_AUDIO_MAX_VOLUME 100
#endif

#define AUDIO_RADIO_I2C_PORT      ((uint8_t)CONFIG_AUDIO_RADIO_I2C_PORT)
#define AUDIO_RADIO_I2C_FREQ_HZ   100000U
#define AUDIO_RADIO_I2S_PORT      I2S_NUM_0
#define AUDIO_RADIO_BITS          16
#define AUDIO_RADIO_CHANNELS      2
#define AUDIO_RADIO_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256
#define AUDIO_RADIO_HLS_BUFFER_SIZE (128 * 1024)
#define AUDIO_RADIO_HLS_TASK_STACK  (20 * 1024)
#define AUDIO_RADIO_RECORD_BUFFER_SIZE 4096
#define AUDIO_RADIO_RECORD_PATH_MAX    256
#define AUDIO_OUTPUT_NVS_NAMESPACE     "audio_out"
#define AUDIO_OUTPUT_NVS_MODE_KEY      "mode"
#define AUDIO_OUTPUT_NVS_SPK_VOLUME_KEY "spk_volume"
#define AUDIO_OUTPUT_NVS_USB_VOLUME_KEY "usb_volume"
#define AUDIO_USB_CONNECT_WAIT_MS      6000
#define AUDIO_USB_WRITE_TIMEOUT_MS     100
// Hot-plug state changes on a human time scale. Five checks per second keeps
// plug/unplug response prompt without running device maintenance every UI tick.
#define AUDIO_OUTPUT_SERVICE_INTERVAL_US 200000

static const char *TAG = "audio_radio";

static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_codec_dev;
static esp_asp_handle_t s_player;
static esp_gmf_io_handle_t s_hls_io;
static bool s_codec_opened;
static bool s_media_adapter_ready;
static bool s_recording;
static volatile bool s_playback_active;
static volatile bool s_playback_ready;
static volatile audio_playback_state_t s_playback_state = AUDIO_PLAYBACK_IDLE;
static uint32_t s_hls_last_format;
static int s_output_volume = CONFIG_AUDIO_RADIO_OUTPUT_VOLUME;
static int s_speaker_volume = CONFIG_AUDIO_RADIO_OUTPUT_VOLUME;
static int s_usb_volume = CONFIG_AUDIO_RADIO_OUTPUT_VOLUME;
static uint32_t s_output_sample_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE;
// s_output_sample_rate is the pipeline's planned PCM rate and may be set in
// prepare_player_cb before the ES8311/I2S sink has actually been reconfigured.
static uint32_t s_codec_sample_rate;
static uint8_t s_output_channels = AUDIO_RADIO_CHANNELS;
static uint8_t s_output_bits = AUDIO_RADIO_BITS;
static uint32_t s_source_sample_rate;
static uint8_t s_source_channels;
static uint8_t s_source_bits;
static bool s_source_format_valid;
static volatile bool s_output_format_ok = true;
// During stop/deinit, acknowledge residual GMF output without touching either
// sink. This prevents an in-flight UAC write from racing stream destruction.
static volatile bool s_output_write_gated;
static portMUX_TYPE s_timing_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_file_pcm_bytes;
static uint32_t s_file_duration_ms;
static volatile bool s_current_source_is_file;
static bool s_extractor_registered;
static uint32_t s_duration_generation;
typedef struct { uint32_t generation; char path[512]; } duration_job_t;
static QueueHandle_t s_duration_queue;
static audio_output_mode_t s_output_mode = AUDIO_OUTPUT_SPEAKER;
static bool s_output_mode_loaded;
static bool s_force_speaker;
static volatile bool s_active_output_usb;
static bool s_last_usb_connected;
static int64_t s_last_output_service_us;
// True while playback is paused only because its USB sink vanished. Separates
// that pause from a deliberate one, so replugging the headset resumes the track
// by itself while a pause the user asked for is left alone.
static volatile bool s_paused_after_usb_loss;

static void audio_radio_load_output_mode(void)
{
    if (s_output_mode_loaded) return;
    s_output_mode_loaded = true;
    nvs_handle_t handle = 0;
    if (nvs_open(AUDIO_OUTPUT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value = AUDIO_OUTPUT_SPEAKER;
    if (nvs_get_u8(handle, AUDIO_OUTPUT_NVS_MODE_KEY, &value) == ESP_OK &&
        value <= AUDIO_OUTPUT_AUTO) {
        s_output_mode = (audio_output_mode_t)value;
    }
    value = CONFIG_AUDIO_RADIO_OUTPUT_VOLUME;
    if (nvs_get_u8(handle, AUDIO_OUTPUT_NVS_SPK_VOLUME_KEY, &value) == ESP_OK &&
        value <= 100) {
        s_speaker_volume = value;
    }
    value = CONFIG_AUDIO_RADIO_OUTPUT_VOLUME;
    if (nvs_get_u8(handle, AUDIO_OUTPUT_NVS_USB_VOLUME_KEY, &value) == ESP_OK &&
        value <= 100) {
        s_usb_volume = value;
    }
    // Codec initialization always starts on the board speaker. If Auto later
    // selects a headset, audio_radio_configure_usb() swaps in the USB value.
    s_output_volume = s_speaker_volume;
    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded output settings: mode=%d speaker_volume=%d usb_volume=%d",
             s_output_mode, s_speaker_volume, s_usb_volume);
}

static void audio_radio_save_output_mode(audio_output_mode_t mode)
{
    nvs_handle_t handle = 0;
    if (nvs_open(AUDIO_OUTPUT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, AUDIO_OUTPUT_NVS_MODE_KEY, (uint8_t)mode) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

void audio_radio_force_speaker(bool force)
{
    s_force_speaker = force;
}

static void audio_radio_save_volume(bool usb, int volume)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(AUDIO_OUTPUT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open audio volume NVS: %s", esp_err_to_name(ret));
        return;
    }
    ret = nvs_set_u8(handle,
                     usb ? AUDIO_OUTPUT_NVS_USB_VOLUME_KEY
                         : AUDIO_OUTPUT_NVS_SPK_VOLUME_KEY,
                     (uint8_t)volume);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save %s volume: %s", usb ? "USB" : "speaker",
                 esp_err_to_name(ret));
    }
}

static int audio_radio_file_read(void *buffer, uint32_t size, void *ctx)
{
    return (int)fread(buffer, 1, size, (FILE *)ctx);
}

static int audio_radio_file_seek(uint32_t position, void *ctx)
{
    return fseek((FILE *)ctx, (long)position, SEEK_SET);
}

static uint32_t audio_radio_file_size(void *ctx)
{
    FILE *file = (FILE *)ctx;
    long previous = ftell(file);
    if (previous < 0 || fseek(file, 0, SEEK_END) != 0) return 0;
    long size = ftell(file);
    if (fseek(file, previous, SEEK_SET) != 0 || size <= 0) return 0;
    return (uint32_t)size;
}

static esp_extractor_type_t audio_radio_extractor_type(const char *path)
{
    const char *extension = path != NULL ? strrchr(path, '.') : NULL;
    if (extension == NULL) return ESP_EXTRACTOR_TYPE_NONE;
    if (strcasecmp(extension, ".wav") == 0) return ESP_EXTRACTOR_TYPE_WAV;
    if (strcasecmp(extension, ".m4a") == 0 || strcasecmp(extension, ".mp4") == 0) {
        return ESP_EXTRACTOR_TYPE_MP4;
    }
    if (strcasecmp(extension, ".ogg") == 0 || strcasecmp(extension, ".opus") == 0) {
        return ESP_EXTRACTOR_TYPE_OGG;
    }
    if (strcasecmp(extension, ".mp3") == 0) return ESP_EXTRACTOR_TYPE_MP3;
    if (strcasecmp(extension, ".aac") == 0) return ESP_EXTRACTOR_TYPE_AAC;
    if (strcasecmp(extension, ".flac") == 0) return ESP_EXTRACTOR_TYPE_FLAC;
    return ESP_EXTRACTOR_TYPE_NONE;
}

static bool audio_radio_read_flac_streaminfo_duration(FILE *file, uint32_t *duration_ms)
{
    if (file == NULL || duration_ms == NULL || fseek(file, 0, SEEK_SET) != 0) return false;

    // A few encoders place an ID3v2 tag before the FLAC marker. Its size is a
    // 28-bit synchsafe integer and does not include the ten-byte header.
    uint8_t prefix[10] = {0};
    if (fread(prefix, 1, sizeof(prefix), file) != sizeof(prefix)) return false;
    long marker_offset = 0;
    if (memcmp(prefix, "ID3", 3) == 0) {
        if ((prefix[6] | prefix[7] | prefix[8] | prefix[9]) & 0x80) return false;
        uint32_t tag_size = ((uint32_t)prefix[6] << 21) |
                            ((uint32_t)prefix[7] << 14) |
                            ((uint32_t)prefix[8] << 7) |
                            (uint32_t)prefix[9];
        marker_offset = 10L + (long)tag_size + ((prefix[5] & 0x10) ? 10L : 0L);
    }

    if (fseek(file, marker_offset, SEEK_SET) != 0) return false;
    uint8_t marker[4] = {0};
    if (fread(marker, 1, sizeof(marker), file) != sizeof(marker) ||
        memcmp(marker, "fLaC", sizeof(marker)) != 0) {
        return false;
    }

    // STREAMINFO is required to be the first FLAC metadata block, but walk a
    // bounded number of blocks as a tolerant fallback for unusual encoders.
    for (int block_index = 0; block_index < 32; ++block_index) {
        uint8_t header[4] = {0};
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) return false;
        bool is_last = (header[0] & 0x80) != 0;
        uint8_t block_type = header[0] & 0x7F;
        uint32_t block_size = ((uint32_t)header[1] << 16) |
                              ((uint32_t)header[2] << 8) |
                              (uint32_t)header[3];

        if (block_type == 0) {
            if (block_size < 34) return false;
            uint8_t streaminfo[34] = {0};
            if (fread(streaminfo, 1, sizeof(streaminfo), file) != sizeof(streaminfo)) {
                return false;
            }
            uint32_t sample_rate = ((uint32_t)streaminfo[10] << 12) |
                                   ((uint32_t)streaminfo[11] << 4) |
                                   ((uint32_t)streaminfo[12] >> 4);
            uint64_t total_samples = ((uint64_t)(streaminfo[13] & 0x0F) << 32) |
                                     ((uint64_t)streaminfo[14] << 24) |
                                     ((uint64_t)streaminfo[15] << 16) |
                                     ((uint64_t)streaminfo[16] << 8) |
                                     (uint64_t)streaminfo[17];
            if (sample_rate == 0 || total_samples == 0) return false;

            uint64_t calculated_ms = (total_samples * 1000u + sample_rate / 2u) / sample_rate;
            *duration_ms = calculated_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)calculated_ms;
            return *duration_ms > 0;
        }

        if (is_last || fseek(file, (long)block_size, SEEK_CUR) != 0) return false;
    }
    return false;
}

typedef struct {
    int64_t begin_us, yield_us;
    uint32_t generation;
    bool background;
} duration_scan_t;
static bool audio_radio_duration_scan_progress(void *ctx)
{
    duration_scan_t *scan = ctx;
    const int64_t now = esp_timer_get_time();
    if (now - scan->begin_us > (scan->background ? 30000000 : 3000000)) return false;
    if (scan->background) {
        portENTER_CRITICAL(&s_timing_lock);
        bool current = scan->generation == s_duration_generation;
        portEXIT_CRITICAL(&s_timing_lock);
        if (!current) return false;
        // Yield between 32KiB SD windows so playback gets first use of the card.
        vTaskDelay(1);
        return true;
    }
    if (now - scan->yield_us >= 100000) {
        vTaskDelay(1);
        scan->yield_us = esp_timer_get_time();
    }
    return true;
}

static void audio_radio_duration_task(void *ctx)
{
    (void)ctx;
    duration_job_t job;
    for (;;) {
        if (xQueueReceive(s_duration_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        duration_scan_t scan = {.begin_us = esp_timer_get_time(),
                               .generation = job.generation, .background = true};
        while (!s_playback_ready && audio_radio_duration_scan_progress(&scan)) vTaskDelay(pdMS_TO_TICKS(20));
        if (!audio_radio_duration_scan_progress(&scan)) continue;
        vTaskDelay(pdMS_TO_TICKS(200)); // Let the decoder/sink prime before metadata I/O.
        FILE *file = fopen(job.path, "rb");
        if (!file) continue;
        mp3_duration_info_t info = {0};
        bool ok = mp3_duration_read(file, &info, audio_radio_duration_scan_progress, &scan);
        fclose(file);
        portENTER_CRITICAL(&s_timing_lock);
        const bool current = job.generation == s_duration_generation && s_current_source_is_file;
        if (ok && current) s_file_duration_ms = info.duration_ms;
        portEXIT_CRITICAL(&s_timing_lock);
        ESP_LOGI(TAG, "MP3 duration background: ok=%d applied=%d duration=%" PRIu32
                 " ms time=%lld ms frames=%" PRIu32 " reads=%" PRIu32 " reason=%s",
                 ok, ok && current, info.duration_ms,
                 (esp_timer_get_time() - scan.begin_us) / 1000,
                 info.frames_scanned, info.read_count, info.failure ? info.failure : "none");
    }
}

static void audio_radio_queue_duration(const char *path)
{
    if (strlen(path) >= sizeof(((duration_job_t *)0)->path)) {
        ESP_LOGW(TAG, "MP3 duration background skipped: path too long");
        return;
    }
    // Requests originate from the serialized playback-control path. Keep one
    // worker and one pending/latest request; never spawn a scanner per switch.
    if (!s_duration_queue) {
        s_duration_queue = xQueueCreate(1, sizeof(duration_job_t));
        if (!s_duration_queue) return;
        if (xTaskCreatePinnedToCoreWithCaps(audio_radio_duration_task, "mp3_duration",
                4096, NULL, 1, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            vQueueDelete(s_duration_queue);
            s_duration_queue = NULL;
            ESP_LOGW(TAG, "MP3 duration background task creation failed");
            return;
        }
    }
    duration_job_t job = {0};
    portENTER_CRITICAL(&s_timing_lock);
    job.generation = s_duration_generation;
    portEXIT_CRITICAL(&s_timing_lock);
    memcpy(job.path, path, strlen(path) + 1);
    xQueueOverwrite(s_duration_queue, &job);
    ESP_LOGI(TAG, "MP3 duration scan queued after playback start");
}

static esp_err_t audio_radio_read_file_duration(const char *path, uint32_t *duration_ms)
{
    ESP_RETURN_ON_FALSE(path != NULL && duration_ms != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "Invalid duration query");
    *duration_ms = 0;

    if (!s_extractor_registered) {
        esp_extractor_err_t register_ret = esp_extractor_register_default();
        ESP_RETURN_ON_FALSE(register_ret == ESP_EXTRACTOR_ERR_OK, ESP_FAIL, TAG,
                            "Failed to register media extractors: %d", register_ret);
        s_extractor_registered = true;
    }

    FILE *file = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(file != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "Failed to open audio metadata: %s", path);

    esp_extractor_type_t extractor_type = audio_radio_extractor_type(path);
    esp_extractor_config_t config = {
        .type = extractor_type,
        .extract_mask = ESP_EXTRACT_MASK_AUDIO,
        .in_read_cb = audio_radio_file_read,
        .in_seek_cb = audio_radio_file_seek,
        .in_size_cb = audio_radio_file_size,
        .in_ctx = file,
        .out_pool_size = 16 * 1024,
        .out_align = 4,
    };
    esp_extractor_handle_t extractor = NULL;
    esp_extractor_err_t extractor_ret = esp_extractor_open(&config, &extractor);
    if (extractor_ret == ESP_EXTRACTOR_ERR_OK) {
        extractor_ret = esp_extractor_parse_stream(extractor);
    }
    if (extractor_ret == ESP_EXTRACTOR_ERR_OK) {
        esp_extractor_stream_info_t info = {0};
        extractor_ret = esp_extractor_get_stream_info(extractor,
                                                       ESP_EXTRACTOR_STREAM_TYPE_AUDIO,
                                                       0, &info);
        if (extractor_ret == ESP_EXTRACTOR_ERR_OK) {
            *duration_ms = info.duration;
            if (info.audio_info.sample_rate > 0 && info.audio_info.channel > 0 &&
                info.audio_info.bits_per_sample > 0) {
                s_source_sample_rate = info.audio_info.sample_rate;
                s_source_channels = info.audio_info.channel;
                s_source_bits = info.audio_info.bits_per_sample;
                s_source_format_valid = true;
                ESP_LOGI(TAG, "Audio source format: %" PRIu32 " Hz, %u ch, %u bit",
                         s_source_sample_rate, s_source_channels, s_source_bits);
            }
        }
    }
    if (extractor != NULL) esp_extractor_close(extractor);

    bool used_flac_streaminfo = false;
    bool used_mp3_fallback = false;
    mp3_duration_info_t mp3_info = {0};
    if ((extractor_ret != ESP_EXTRACTOR_ERR_OK || *duration_ms == 0) &&
        extractor_type == ESP_EXTRACTOR_TYPE_FLAC) {
        used_flac_streaminfo = audio_radio_read_flac_streaminfo_duration(file, duration_ms);
    }
    if ((extractor_ret != ESP_EXTRACTOR_ERR_OK || *duration_ms == 0) &&
        extractor_type == ESP_EXTRACTOR_TYPE_MP3) {
        duration_scan_t scan = {.begin_us = esp_timer_get_time()};
        scan.yield_us = scan.begin_us;
        used_mp3_fallback = mp3_duration_read_tags(file, &mp3_info,
                                             audio_radio_duration_scan_progress, &scan);
        if (used_mp3_fallback) {
            *duration_ms = mp3_info.duration_ms;
        }
        ESP_LOGI(TAG, "MP3 duration fallback: ok=%d source=%s duration=%" PRIu32 " ms time=%lld ms stage=%s reason=%s first=%ld frames=%" PRIu32 " reads=%" PRIu32 " bytes=%" PRIu64,
                 used_mp3_fallback, used_mp3_fallback ? mp3_info.source : "unavailable",
                 mp3_info.duration_ms, (esp_timer_get_time() - scan.begin_us) / 1000,
                 mp3_info.stage ? mp3_info.stage : "none", mp3_info.failure ? mp3_info.failure : "none",
                 mp3_info.first_frame_offset, mp3_info.frames_scanned, mp3_info.read_count, mp3_info.read_bytes);
    }
    fclose(file);

    if (!used_mp3_fallback && extractor_type == ESP_EXTRACTOR_TYPE_MP3 &&
        *duration_ms == 0 && mp3_info.failure && !strcmp(mp3_info.failure, "no-duration-tag")) {
        return ESP_ERR_NOT_FINISHED; // Full frame scan is deferred until playback starts.
    }

    if (!used_flac_streaminfo && !used_mp3_fallback &&
        (extractor_ret != ESP_EXTRACTOR_ERR_OK || *duration_ms == 0)) {
        ESP_LOGW(TAG, "Audio duration unavailable for %s, extractor ret=%d",
                 path, extractor_ret);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "Audio metadata duration=%" PRIu32 " ms source=%s path=%s",
             *duration_ms, used_flac_streaminfo ? "flac-streaminfo" :
                           used_mp3_fallback ? mp3_info.source : "extractor", path);
    return ESP_OK;
}

static esp_err_t audio_radio_set_pa(bool enable)
{
    gpio_num_t pa_gpio = (gpio_num_t)CONFIG_AUDIO_RADIO_PA_CTRL_GPIO;
    ESP_RETURN_ON_ERROR(gpio_reset_pin(pa_gpio), TAG, "Failed to reset PA GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_direction(pa_gpio, GPIO_MODE_OUTPUT), TAG, "Failed to configure PA GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(pa_gpio, enable ? 1 : 0), TAG, "Failed to set PA GPIO");
    ESP_LOGI(TAG, "PA %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

static esp_err_t audio_radio_codec_ret_to_esp(int ret)
{
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_radio_set_output_sample_rate(uint32_t sample_rate);
static esp_err_t audio_radio_set_output_format(uint32_t sample_rate,
                                               uint8_t channels, uint8_t bits);
static esp_err_t audio_radio_fall_back_to_speaker(uint32_t *target_rate);

static int audio_radio_write_cb(uint8_t *pcm, int len, void *ctx)
{
    (void)ctx;
    if (pcm == NULL || len <= 0 || !s_output_format_ok) {
        return -1;
    }
    if (s_output_write_gated) {
        return len;
    }
    bool write_ok = false;
    if (s_active_output_usb) {
        // UAC hardware volume is optional. Scale signed 16-bit PCM in place so
        // every compliant headset follows the application's volume setting.
        // USB has its own ceiling because headphone sensitivity is generally
        // much higher than the board speaker path.
        // Keep two decimal places of percentage precision. With a low ceiling
        // such as 5%, calculating the effective percentage first would make
        // several UI volume steps collapse to zero after integer truncation.
        const int usb_gain_basis_points =
            s_output_volume * CONFIG_MYPAPER_USB_AUDIO_MAX_VOLUME;
        if (usb_audio_output_uses_software_volume() && usb_gain_basis_points < 10000) {
            if (s_output_bits == 24) {
                for (size_t i = 0; i + 2 < (size_t)len; i += 3) {
                    int32_t sample = (int32_t)pcm[i] |
                                     ((int32_t)pcm[i + 1] << 8) |
                                     ((int32_t)pcm[i + 2] << 16);
                    if (sample & 0x00800000) sample |= (int32_t)0xff000000;
                    sample = (int32_t)(((int64_t)sample * usb_gain_basis_points) / 10000);
                    pcm[i] = (uint8_t)sample;
                    pcm[i + 1] = (uint8_t)(sample >> 8);
                    pcm[i + 2] = (uint8_t)(sample >> 16);
                }
            } else if (s_output_bits == 32) {
                int32_t *samples = (int32_t *)pcm;
                size_t sample_count = (size_t)len / sizeof(int32_t);
                for (size_t i = 0; i < sample_count; ++i) {
                    samples[i] = (int32_t)(((int64_t)samples[i] * usb_gain_basis_points) /
                                           10000);
                }
            } else {
            int16_t *samples = (int16_t *)pcm;
            size_t sample_count = (size_t)len / sizeof(int16_t);
            for (size_t i = 0; i < sample_count; ++i) {
                samples[i] = (int16_t)(((int32_t)samples[i] * usb_gain_basis_points) /
                                       10000);
            }
            }
        }
        write_ok = usb_audio_output_write(pcm, (size_t)len,
                                           AUDIO_USB_WRITE_TIMEOUT_MS) == ESP_OK;
    } else if (s_codec_dev != NULL) {
        write_ok = esp_codec_dev_write(s_codec_dev, pcm, len) == ESP_CODEC_DEV_OK;
    }
    if (write_ok && s_current_source_is_file) {
        portENTER_CRITICAL(&s_timing_lock);
        s_file_pcm_bytes += (uint32_t)len;
        portEXIT_CRITICAL(&s_timing_lock);
    }
    // A failed sink write must not be fatal to the pipeline. The write that is in
    // flight when the headset is pulled always fails - it is already queued
    // against a device that is gone - and returning -1 here tears the whole
    // decode pipeline down (ESP_GMF_BIT_CVT reports "OUT port release error"),
    // which nothing can undo: stop, pause and resume are all refused once the
    // pipeline sits in ESP_GMF_EVENT_STATE_ERROR. That would cost us the one
    // thing worth keeping across an unplug, the decode position, and
    // esp_gmf_pipeline_seek cannot restore it for FLAC. Consume the samples
    // instead; audio_radio_service_output() sees the missing output on its next
    // poll and pauses playback, which stays resumable.
    return len;
}

static int audio_radio_player_event_cb(esp_asp_event_pkt_t *pkt, void *ctx)
{
    (void) ctx;

    if (pkt == NULL) {
        return 0;
    }
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE && pkt->payload != NULL
        && pkt->payload_size == sizeof(esp_asp_state_t)) {
        esp_asp_state_t state = *(esp_asp_state_t *)pkt->payload;
        ESP_LOGI(TAG, "Player state: %s", esp_audio_simple_player_state_to_str(state));
        if (state == ESP_ASP_STATE_RUNNING || state == ESP_ASP_STATE_PAUSED) {
            s_playback_active = true;
        } else if (state == ESP_ASP_STATE_STOPPED ||
                   state == ESP_ASP_STATE_FINISHED ||
                   state == ESP_ASP_STATE_ERROR) {
            s_playback_active = false;
        }
        switch (state) {
            case ESP_ASP_STATE_RUNNING:
                s_playback_state = AUDIO_PLAYBACK_PLAYING;
                s_playback_ready = true;
                break;
            case ESP_ASP_STATE_PAUSED:
                s_playback_state = AUDIO_PLAYBACK_PAUSED;
                break;
            case ESP_ASP_STATE_STOPPED:
                s_playback_state = AUDIO_PLAYBACK_STOPPED;
                s_playback_ready = false;
                break;
            case ESP_ASP_STATE_FINISHED:
                s_playback_state = AUDIO_PLAYBACK_FINISHED;
                s_playback_ready = false;
                break;
            case ESP_ASP_STATE_ERROR:
                s_playback_state = AUDIO_PLAYBACK_ERROR;
                s_playback_ready = false;
                break;
            default:
                break;
        }
    } else if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO && pkt->payload != NULL) {
        esp_asp_music_info_t *info = (esp_asp_music_info_t *)pkt->payload;
        ESP_LOGI(TAG, "Music output: %d Hz, %d ch, %d bit, bitrate=%d",
                 (int)info->sample_rate,
                 (int)info->channels,
                 (int)info->bits,
                 (int)info->bitrate);
        if (s_source_format_valid &&
            ((uint32_t)info->sample_rate != s_output_sample_rate ||
             (uint8_t)info->channels != s_output_channels ||
             (uint8_t)info->bits != s_output_bits)) {
            ESP_LOGD(TAG, "Ignoring intermediate PCM report; waiting for %" PRIu32
                     " Hz/%u ch/%u bit", s_output_sample_rate,
                     s_output_channels, s_output_bits);
            return 0;
        }
        esp_err_t ret = audio_radio_set_output_format((uint32_t)info->sample_rate,
                                                      (uint8_t)info->channels,
                                                      (uint8_t)info->bits);
        if (ret != ESP_OK) {
            s_output_format_ok = false;
            ESP_LOGE(TAG, "Failed to configure audio output for %d Hz: %s",
                     (int)info->sample_rate, esp_err_to_name(ret));
            return -1;
        }
    }

    return 0;
}

static int audio_radio_hls_media_type_cb(esp_hls_file_seg_info_t *info, void *ctx)
{
    if (info == NULL || ctx == NULL) {
        return -1;
    }
    if (info->format == 0 || info->format == UINT32_MAX) {
        ESP_LOGE(TAG, "HLS reported an invalid audio format: 0x%08" PRIx32, info->format);
        return -1;
    }
    if (info->format == s_hls_last_format) {
        return 0;
    }

    esp_asp_handle_t player = (esp_asp_handle_t)ctx;
    esp_gmf_pipeline_handle_t pipeline = NULL;
    esp_gmf_element_handle_t dec_el = NULL;
    esp_audio_simple_player_get_pipeline(player, &pipeline);
    if (pipeline == NULL) {
        return -1;
    }
    esp_gmf_pipeline_get_el_by_name(pipeline, "aud_dec", &dec_el);
    if (dec_el == NULL) {
        return -1;
    }

    esp_gmf_info_sound_t sound_info = {
        .format_id = info->format,
        .sample_rates = CONFIG_AUDIO_RADIO_SAMPLE_RATE,
        .channels = AUDIO_RADIO_CHANNELS,
        .bits = AUDIO_RADIO_BITS,
    };
    ESP_LOGI(TAG, "HLS segment audio format: %c%c%c%c",
             (char)(info->format & 0xff),
             (char)((info->format >> 8) & 0xff),
             (char)((info->format >> 16) & 0xff),
             (char)((info->format >> 24) & 0xff));
    if (esp_gmf_audio_dec_reconfig_by_sound_info(dec_el, &sound_info) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to reconfigure audio decoder");
        return -1;
    }
    s_hls_last_format = info->format;
    return 0;
}

static void audio_radio_write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = value & 0xff;
    dst[1] = (value >> 8) & 0xff;
}

static void audio_radio_write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = value & 0xff;
    dst[1] = (value >> 8) & 0xff;
    dst[2] = (value >> 16) & 0xff;
    dst[3] = (value >> 24) & 0xff;
}

static esp_err_t audio_radio_write_wav_header(FILE *file, uint32_t data_size)
{
    uint8_t header[44] = {0};
    const uint16_t channels = AUDIO_RADIO_CHANNELS;
    const uint16_t bits = AUDIO_RADIO_BITS;
    const uint32_t sample_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t byte_rate = sample_rate * block_align;

    memcpy(&header[0], "RIFF", 4);
    audio_radio_write_le32(&header[4], 36 + data_size);
    memcpy(&header[8], "WAVE", 4);
    memcpy(&header[12], "fmt ", 4);
    audio_radio_write_le32(&header[16], 16);
    audio_radio_write_le16(&header[20], 1);
    audio_radio_write_le16(&header[22], channels);
    audio_radio_write_le32(&header[24], sample_rate);
    audio_radio_write_le32(&header[28], byte_rate);
    audio_radio_write_le16(&header[32], block_align);
    audio_radio_write_le16(&header[34], bits);
    memcpy(&header[36], "data", 4);
    audio_radio_write_le32(&header[40], data_size);

    return fwrite(header, 1, sizeof(header), file) == sizeof(header) ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_radio_normalize_record_path(const char *path, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(path != NULL && path[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "Record path is empty");

    if (strncmp(path, "file://", strlen("file://")) == 0) {
        path += strlen("file://");
        if (path[0] != '/') {
            int written = snprintf(out, out_size, "/%s", path);
            ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_size,
                                ESP_ERR_INVALID_SIZE, TAG, "Record path is too long");
            return ESP_OK;
        }
    }

    int written = snprintf(out, out_size, "%s", path);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_size,
                        ESP_ERR_INVALID_SIZE, TAG, "Record path is too long");
    return ESP_OK;
}

static esp_err_t audio_radio_init_i2s(void)
{
    if (s_i2s_tx != NULL && s_i2s_rx != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_RADIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG, "Failed to create I2S channels");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_AUDIO_RADIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_AUDIO_RADIO_I2S_MCLK_GPIO,
            .bclk = CONFIG_AUDIO_RADIO_I2S_SCLK_GPIO,
            .ws = CONFIG_AUDIO_RADIO_I2S_LRCK_GPIO,
            .dout = CONFIG_AUDIO_RADIO_I2S_DSDIN_GPIO,
            .din = CONFIG_AUDIO_RADIO_I2S_ASDOUT_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_RADIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "Failed to initialize I2S STD mode");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG, "Failed to initialize I2S RX STD mode");
    return ESP_OK;
}

static esp_err_t audio_radio_init_codec(void)
{
    if (s_codec_dev != NULL) {
        ESP_RETURN_ON_ERROR(audio_radio_set_pa(true), TAG, "Failed to enable PA");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(audio_radio_set_pa(false), TAG, "Failed to disable PA before codec init");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_RADIO_I2S_PORT,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_FAIL, TAG, "Failed to create codec I2S data interface");

    // ES8311_CODEC_DEFAULT_ADDR is the esp_codec_dev 8-bit address (0x30).
    // M5Unified uses the standard 7-bit address (0x18).
    s_ctrl_if = audio_codec_new_m5_i2c_ctrl(AUDIO_RADIO_I2C_PORT,
                                            ES8311_CODEC_DEFAULT_ADDR >> 1,
                                            AUDIO_RADIO_I2C_FREQ_HZ);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_FAIL, TAG,
                        "Failed to create M5Unified codec I2C interface");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_FAIL, TAG, "Failed to create codec GPIO interface");

    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .pa_pin = CONFIG_AUDIO_RADIO_PA_CTRL_GPIO,
        .use_mclk = true,
        .no_dac_ref = true,
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_FAIL, TAG, "Failed to create ES8311 codec interface");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_FAIL, TAG, "Failed to create codec device");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE,
        .channel = AUDIO_RADIO_CHANNELS,
        .bits_per_sample = AUDIO_RADIO_BITS,
        .mclk_multiple = AUDIO_RADIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_open(s_codec_dev, &fs)),
                        TAG,
                        "Failed to open codec device");
    s_codec_opened = true;
    s_codec_sample_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE;
    s_output_sample_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE;

    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_vol(s_codec_dev, s_output_volume)),
                        TAG,
                        "Failed to set output volume");
    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_mute(s_codec_dev, false)),
                        TAG,
                        "Failed to unmute output");
    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_in_gain(s_codec_dev, CONFIG_AUDIO_RADIO_INPUT_GAIN_DB)),
                        TAG,
                        "Failed to set input gain");
    int mute_ret = esp_codec_dev_set_in_mute(s_codec_dev, false);
    if (mute_ret == ESP_CODEC_DEV_NOT_SUPPORT) {
        ESP_LOGW(TAG, "Input mute control is not supported by this ES8311 driver, skip it");
    } else {
        ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(mute_ret), TAG, "Failed to unmute input");
    }

    ESP_RETURN_ON_ERROR(audio_radio_set_pa(true), TAG, "Failed to enable PA");
    return ESP_OK;
}

static esp_err_t audio_radio_configure_speaker(uint32_t sample_rate)
{
    ESP_RETURN_ON_FALSE(sample_rate > 0 && sample_rate <= CONFIG_AUDIO_RADIO_SAMPLE_RATE,
                        ESP_ERR_INVALID_ARG, TAG, "Unsupported output sample rate: %" PRIu32, sample_rate);
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "Codec is not initialized");
    s_output_volume = s_speaker_volume;
    if (s_codec_opened && sample_rate == s_codec_sample_rate) {
        ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(
                                esp_codec_dev_set_out_vol(s_codec_dev, s_output_volume)),
                            TAG, "Failed to restore speaker volume");
        ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(
                                esp_codec_dev_set_out_mute(s_codec_dev, false)),
                            TAG, "Failed to unmute speaker output");
        ESP_RETURN_ON_ERROR(audio_radio_set_pa(true), TAG, "Failed to enable PA");
        s_output_format_ok = true;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Reconfiguring I2S/ES8311 output: %" PRIu32 " -> %" PRIu32 " Hz",
             s_codec_sample_rate, sample_rate);
    if (s_codec_opened) {
        esp_codec_dev_set_out_mute(s_codec_dev, true);
        audio_radio_set_pa(false);
        ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_close(s_codec_dev)),
                            TAG, "Failed to close codec for sample-rate change");
        s_codec_opened = false;
        s_codec_sample_rate = 0;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = AUDIO_RADIO_CHANNELS,
        .bits_per_sample = AUDIO_RADIO_BITS,
        .mclk_multiple = AUDIO_RADIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_open(s_codec_dev, &fs)),
                        TAG, "Failed to reopen codec at requested sample rate");
    s_codec_opened = true;
    s_codec_sample_rate = sample_rate;
    s_output_sample_rate = sample_rate;
    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_vol(s_codec_dev, s_output_volume)),
                        TAG, "Failed to restore output volume");
    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_mute(s_codec_dev, false)),
                        TAG, "Failed to unmute reconfigured output");
    ESP_RETURN_ON_ERROR(audio_radio_set_pa(true), TAG, "Failed to enable PA after sample-rate change");
    s_output_format_ok = true;
    return ESP_OK;
}

static esp_err_t audio_radio_configure_usb(uint32_t sample_rate, uint8_t channels,
                                           uint8_t bits, bool wait_for_device)
{
    ESP_RETURN_ON_ERROR(usb_audio_output_init(), TAG, "USB audio host init failed");
    if (wait_for_device && !usb_audio_output_wait_connected(AUDIO_USB_CONNECT_WAIT_MS)) {
        ESP_LOGW(TAG, "No compatible USB audio playback interface detected");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_FALSE(usb_audio_output_supports(sample_rate, channels, bits),
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "USB headset does not support %" PRIu32 " Hz, %u ch, %u-bit PCM",
                        sample_rate, channels, bits);
    ESP_RETURN_ON_ERROR(usb_audio_output_start(sample_rate, channels, bits),
                        TAG, "Failed to start USB audio stream");
    s_output_volume = s_usb_volume;
    // UAC2 Feature Unit is preferred. The output layer switches to software
    // PCM gain if any hardware control request fails.
    (void)usb_audio_output_set_volume((uint8_t)s_output_volume);
    if (s_codec_dev != NULL && s_codec_opened) {
        (void)esp_codec_dev_set_out_mute(s_codec_dev, true);
    }
    (void)audio_radio_set_pa(false);
    s_output_sample_rate = sample_rate;
    s_output_channels = channels;
    s_output_bits = bits;
    s_active_output_usb = true;
    s_output_format_ok = true;
    ESP_LOGI(TAG, "Active audio output: USB (%" PRIu32 " Hz)", sample_rate);
    return ESP_OK;
}

static esp_err_t audio_radio_set_output_format(uint32_t sample_rate,
                                               uint8_t channels, uint8_t bits)
{
    ESP_RETURN_ON_FALSE(sample_rate > 0 && channels > 0 && bits > 0,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid output PCM format");
    audio_radio_load_output_mode();

    if (!s_force_speaker && s_output_mode != AUDIO_OUTPUT_SPEAKER) {
        esp_err_t usb_ret = audio_radio_configure_usb(
            sample_rate, channels, bits, s_output_mode == AUDIO_OUTPUT_USB);
        if (usb_ret == ESP_OK) return ESP_OK;
        if (s_output_mode == AUDIO_OUTPUT_USB) return usb_ret;
        ESP_LOGW(TAG, "USB unavailable in Auto mode; using ES8311: %s",
                 esp_err_to_name(usb_ret));
    }

    (void)usb_audio_output_stop();
    s_active_output_usb = false;
    ESP_RETURN_ON_FALSE(channels == AUDIO_RADIO_CHANNELS && bits == AUDIO_RADIO_BITS &&
                            sample_rate <= CONFIG_AUDIO_RADIO_SAMPLE_RATE,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "Speaker requires <=%d Hz stereo 16-bit PCM",
                        CONFIG_AUDIO_RADIO_SAMPLE_RATE);
    esp_err_t ret = audio_radio_configure_speaker(sample_rate);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Active audio output: speaker (%" PRIu32 " Hz)", sample_rate);
    }
    return ret;
}

static esp_err_t audio_radio_set_output_sample_rate(uint32_t sample_rate)
{
    return audio_radio_set_output_format(sample_rate, s_output_channels, s_output_bits);
}

static int audio_radio_prepare_player_cb(esp_asp_handle_t *handle, void *ctx)
{
    (void)ctx;
    esp_gmf_pipeline_handle_t pipeline = NULL;
    esp_gmf_element_handle_t rate_el = NULL;
    esp_gmf_element_handle_t channel_el = NULL;
    esp_gmf_element_handle_t bit_el = NULL;
    if (esp_audio_simple_player_get_pipeline((esp_asp_handle_t)handle, &pipeline) != ESP_GMF_ERR_OK ||
        pipeline == NULL ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_rate_cvt", &rate_el) != ESP_GMF_ERR_OK ||
        rate_el == NULL ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_ch_cvt", &channel_el) != ESP_GMF_ERR_OK ||
        channel_el == NULL ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_bit_cvt", &bit_el) != ESP_GMF_ERR_OK ||
        bit_el == NULL) {
        ESP_LOGE(TAG, "Failed to find PCM converters before playback");
        return -1;
    }
    audio_radio_load_output_mode();
    bool use_usb_format = false;
    const uint32_t source_rate = s_source_format_valid
                                     ? s_source_sample_rate
                                     : CONFIG_AUDIO_RADIO_SAMPLE_RATE;
    const uint8_t source_channels = s_source_format_valid
                                        ? s_source_channels
                                        : AUDIO_RADIO_CHANNELS;
    const uint8_t source_bits = s_source_format_valid ? s_source_bits : AUDIO_RADIO_BITS;
    uint32_t usb_rate = 0;
    uint8_t usb_channels = 0;
    uint8_t usb_bits = 0;
    if (!s_force_speaker && s_output_mode != AUDIO_OUTPUT_SPEAKER) {
        if (s_output_mode != AUDIO_OUTPUT_USB ||
            usb_audio_output_wait_connected(AUDIO_USB_CONNECT_WAIT_MS)) {
            use_usb_format = usb_audio_output_select_format(
                source_rate, source_channels, source_bits, 0,
                &usb_rate, &usb_channels, &usb_bits);
        }
        if (s_output_mode == AUDIO_OUTPUT_USB && !use_usb_format) {
            ESP_LOGE(TAG, "No USB PCM format for source %" PRIu32 " Hz/%u ch/%u bit",
                     source_rate, source_channels, source_bits);
            return -1;
        }
    }

    uint8_t target_channels = AUDIO_RADIO_CHANNELS;
    uint8_t target_bits = AUDIO_RADIO_BITS;
    if (use_usb_format) {
        if (esp_gmf_rate_cvt_clear_max_dest_rate(rate_el) != ESP_GMF_ERR_OK ||
            esp_gmf_rate_cvt_set_dest_rate(rate_el, usb_rate) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "Failed to force USB destination rate");
            return -1;
        }
        target_channels = usb_channels;
        target_bits = usb_bits;
        ESP_LOGI(TAG, "USB PCM target: source=%" PRIu32 "/%u/%u target=%" PRIu32
                 "/%u/%u (no USB rate ceiling)", source_rate, source_channels,
                 source_bits, usb_rate, usb_channels, usb_bits);
    } else if (esp_gmf_rate_cvt_set_max_dest_rate(
                   rate_el, CONFIG_AUDIO_RADIO_SAMPLE_RATE) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to set adaptive sample-rate cap");
        return -1;
    }
    if (esp_gmf_ch_cvt_set_dest_channel(channel_el, target_channels) != ESP_GMF_ERR_OK ||
        esp_gmf_bit_cvt_set_dest_bits(bit_el, target_bits) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to configure USB PCM channel/bit converters");
        return -1;
    }
    s_output_sample_rate = use_usb_format ? usb_rate
                                         : (source_rate > CONFIG_AUDIO_RADIO_SAMPLE_RATE
                                                ? CONFIG_AUDIO_RADIO_SAMPLE_RATE
                                                : source_rate);
    s_output_channels = target_channels;
    s_output_bits = target_bits;
    return 0;
}

static esp_err_t audio_radio_init_player(void)
{
    if (s_player != NULL) {
        return ESP_OK;
    }

    if (!s_media_adapter_ready) {
        media_lib_add_default_adapter();
        s_media_adapter_ready = true;
    }

    esp_asp_cfg_t player_cfg = {
        .out = {
            .cb = audio_radio_write_cb,
            .user_ctx = NULL,
        },
        .task_prio = 5,
        .task_stack = 6 * 1024,
        .task_stack_in_ext = true,
        .prev = audio_radio_prepare_player_cb,
        .prev_ctx = NULL,
    };
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_new(&player_cfg, &s_player) == ESP_GMF_ERR_OK,
                        ESP_FAIL,
                        TAG, "Failed to create audio simple player");

    esp_gmf_pool_handle_t pool = NULL;
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_get_pool(s_player, &pool) == ESP_GMF_ERR_OK && pool != NULL,
                        ESP_FAIL,
                        TAG, "Failed to get simple player pool");

    esp_hls_io_cfg_t hls_cfg = {
        .pool = pool,
        .name = "io_hls",
        .file_seg_cb = audio_radio_hls_media_type_cb,
        .ctx = s_player,
        .io_cfg = {
            .thread = {
                .stack = AUDIO_RADIO_HLS_TASK_STACK,
                // Keep playlist/segment feeding above the decode pipeline.
                // Dropping this below the player can starve PCM delivery and
                // produces audible gaps on both UAC1 and UAC2. Radio UI runs at
                // the nested HTTP reader's priority (10), so it remains
                // responsive without weakening this producer.
                .prio = 9,
                // Core 1 is reserved for the EPD update/scan pipeline.
                .core = 0,
                .stack_in_ext = true,
            },
            .buffer_cfg = {
                .buffer_size = AUDIO_RADIO_HLS_BUFFER_SIZE,
            },
        },
    };
    ESP_RETURN_ON_FALSE(esp_gmf_io_hls_init(&hls_cfg, &s_hls_io) == ESP_GMF_ERR_OK,
                        ESP_FAIL,
                        TAG, "Failed to initialize HLS IO");
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_register_io(s_player, s_hls_io) == ESP_GMF_ERR_OK,
                        ESP_FAIL,
                        TAG, "Failed to register HLS IO");
    esp_audio_simple_player_set_event(s_player, audio_radio_player_event_cb, NULL);
    s_hls_last_format = 0;
    return ESP_OK;
}

esp_err_t audio_radio_init(void)
{
    audio_radio_load_output_mode();
    ESP_RETURN_ON_ERROR(audio_radio_init_i2s(), TAG, "I2S init failed");
    ESP_RETURN_ON_ERROR(audio_radio_init_codec(), TAG, "Codec init failed");
    if (!s_force_speaker && s_output_mode != AUDIO_OUTPUT_SPEAKER) {
        esp_err_t usb_ret = usb_audio_output_init();
        if (usb_ret != ESP_OK && s_output_mode == AUDIO_OUTPUT_USB) return usb_ret;
    }
    ESP_RETURN_ON_ERROR(audio_radio_init_player(), TAG, "Player init failed");
    return ESP_OK;
}

esp_err_t audio_radio_play(const char *url)
{
    ESP_RETURN_ON_FALSE(url != NULL && url[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "Radio URL is empty");

    /*
     * The HLS IO owns worker tasks and a data bus separate from the decoder
     * pipeline. Reusing it immediately after stop can let metadata from the
     * old stream reach the next run. Recreate only the player/HLS layer when
     * switching; the codec, I2S and shared I2C bus remain initialized.
     */
    if (s_player != NULL) {
        ESP_RETURN_ON_ERROR(audio_radio_stop(), TAG, "Failed to stop current station");
        ESP_RETURN_ON_FALSE(esp_audio_simple_player_destroy(s_player) == ESP_GMF_ERR_OK,
                            ESP_FAIL, TAG, "Failed to destroy previous HLS player");
        s_player = NULL;
        s_hls_io = NULL;
        s_playback_active = false;
        s_hls_last_format = 0;
    }
    ESP_RETURN_ON_ERROR(audio_radio_init(), TAG, "Audio radio init failed");

    ESP_LOGI(TAG, "Playing radio URL: %s", url);
    const bool is_file = strncmp(url, "file://", 7) == 0;
    if (!is_file) s_source_format_valid = false;
    s_playback_ready = false;
    portENTER_CRITICAL(&s_timing_lock);
    s_file_pcm_bytes = 0;
    s_file_duration_ms = 0;
    ++s_duration_generation;
    s_current_source_is_file = is_file;
    portEXIT_CRITICAL(&s_timing_lock);
    s_output_format_ok = true;
    s_output_write_gated = false;
    esp_asp_music_info_t source_info = {0};
    esp_asp_music_info_t *source_info_ptr = NULL;
    if (is_file && s_source_format_valid) {
        source_info.sample_rate = s_source_sample_rate;
        source_info.channels = s_source_channels;
        source_info.bits = s_source_bits;
        source_info_ptr = &source_info;
    }
    int run_ret = esp_audio_simple_player_run(s_player, url, source_info_ptr);
    if (run_ret != ESP_GMF_ERR_OK) {
        s_output_write_gated = true;
        s_current_source_is_file = false;
        s_playback_ready = false;
        s_playback_active = false;
        s_playback_state = AUDIO_PLAYBACK_ERROR;
        ESP_LOGE(TAG, "Failed to start audio playback: %d", run_ret);
        return ESP_FAIL;
    }
    s_playback_active = true;
    s_playback_state = AUDIO_PLAYBACK_PLAYING;
    // A fresh pipeline has no history to claim back, and any unplug this session
    // has already been accounted for by the source it just picked.
    s_paused_after_usb_loss = false;
    return ESP_OK;
}

esp_err_t audio_radio_play_file(const char *path)
{
    ESP_RETURN_ON_FALSE(path != NULL && path[0] == '/', ESP_ERR_INVALID_ARG,
                        TAG, "Audio file path must be absolute");

    if (s_player != NULL) {
        ESP_RETURN_ON_ERROR(audio_radio_stop(), TAG,
                            "Failed to stop current audio before reading metadata");
    }

    s_source_format_valid = false;
    s_source_sample_rate = 0;
    s_source_channels = 0;
    s_source_bits = 0;
    uint32_t duration_ms = 0;
    if (audio_radio_read_file_duration(path, &duration_ms) != ESP_OK) {
        // Duration is presentation metadata; playback remains available when
        // a damaged or unusual file cannot be parsed by the extractor.
        duration_ms = 0;
    }

    size_t uri_size = strlen("file://") + strlen(path) + 1;
    char *uri = malloc(uri_size);
    ESP_RETURN_ON_FALSE(uri != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate audio file URI");
    snprintf(uri, uri_size, "file://%s", path);
    esp_err_t ret = audio_radio_play(uri);
    free(uri);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_timing_lock);
        s_file_duration_ms = duration_ms;
        portEXIT_CRITICAL(&s_timing_lock);
        if (duration_ms == 0 && audio_radio_extractor_type(path) == ESP_EXTRACTOR_TYPE_MP3) {
            audio_radio_queue_duration(path);
        }
    }
    return ret;
}

esp_err_t audio_radio_play_default(void)
{
    ESP_LOGI(TAG, "Default station: %s", CONFIG_AUDIO_RADIO_NAME);
    return audio_radio_play(CONFIG_AUDIO_RADIO_URL);
}

esp_err_t audio_radio_stop(void)
{
    portENTER_CRITICAL(&s_timing_lock);
    ++s_duration_generation; // Cancels an in-flight scan without waiting on SD I/O.
    portEXIT_CRITICAL(&s_timing_lock);
    const int64_t stop_begin_us = esp_timer_get_time();
    // Close the output gate before stopping a sink. A callback already inside
    // usb_audio_output_write() is bounded by AUDIO_USB_WRITE_TIMEOUT_MS; all
    // subsequent callbacks are consumed without reaching the UAC device.
    s_output_write_gated = true;
    if (s_active_output_usb) {
        const int64_t usb_begin_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Stop: USB stream begin");
        esp_err_t usb_ret = usb_audio_output_stop();
        ESP_LOGI(TAG, "Stop: USB stream complete ret=%s elapsed=%lld ms",
                 esp_err_to_name(usb_ret),
                 (long long)((esp_timer_get_time() - usb_begin_us) / 1000));
        if (usb_ret != ESP_OK) {
            ESP_LOGW(TAG, "USB stream stop failed: %s", esp_err_to_name(usb_ret));
        }
    }

    s_paused_after_usb_loss = false;
    if (s_player == NULL) {
        s_playback_active = false;
        s_playback_ready = false;
        return ESP_OK;
    }

    esp_asp_state_t state = ESP_ASP_STATE_NONE;
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_get_state(s_player, &state) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Failed to query playback state");
    if (!s_playback_active && state != ESP_ASP_STATE_RUNNING && state != ESP_ASP_STATE_PAUSED) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping current audio playback");
    const int64_t gmf_begin_us = esp_timer_get_time();
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_stop(s_player) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Failed to stop playback");
    ESP_LOGI(TAG, "Stop: GMF complete elapsed=%lld ms",
             (long long)((esp_timer_get_time() - gmf_begin_us) / 1000));
    s_playback_active = false;
    s_playback_ready = false;
    s_playback_state = AUDIO_PLAYBACK_STOPPED;
    ESP_LOGI(TAG, "Stop complete total=%lld ms",
             (long long)((esp_timer_get_time() - stop_begin_us) / 1000));
    return ESP_OK;
}

esp_err_t audio_radio_pause(void)
{
    ESP_RETURN_ON_FALSE(s_player != NULL, ESP_ERR_INVALID_STATE, TAG, "Player is not initialized");
    ESP_RETURN_ON_FALSE(s_playback_state == AUDIO_PLAYBACK_PLAYING,
                        ESP_ERR_INVALID_STATE, TAG, "Playback is not running");
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_pause(s_player) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Failed to pause playback");
    s_playback_active = true;
    s_playback_state = AUDIO_PLAYBACK_PAUSED;
    return ESP_OK;
}

esp_err_t audio_radio_resume(void)
{
    ESP_RETURN_ON_FALSE(s_player != NULL, ESP_ERR_INVALID_STATE, TAG, "Player is not initialized");
    ESP_RETURN_ON_FALSE(s_playback_state == AUDIO_PLAYBACK_PAUSED,
                        ESP_ERR_INVALID_STATE, TAG, "Playback is not paused");
    // A pause caused by losing the USB sink leaves the pipeline pointed at an
    // output that is gone. Move it to a live one before resuming, otherwise the
    // very write that caused the pause fails again. Skipped when a headset has
    // since been replugged and audio_radio_service_output() already claimed it.
    if (s_active_output_usb && !audio_radio_usb_connected()) {
        uint32_t target_rate = s_output_sample_rate;
        ESP_RETURN_ON_ERROR(audio_radio_fall_back_to_speaker(&target_rate), TAG,
                            "Failed to move playback off the missing USB output");
    }
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_resume(s_player) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Failed to resume playback");
    s_playback_active = true;
    s_playback_state = AUDIO_PLAYBACK_PLAYING;
    s_paused_after_usb_loss = false;
    return ESP_OK;
}

audio_playback_state_t audio_radio_get_state(void)
{
    return s_playback_state;
}

bool audio_radio_is_playback_ready(void)
{
    return s_playback_ready;
}

esp_err_t audio_radio_get_file_progress(uint32_t *progress_permille)
{
    if (progress_permille == NULL) return ESP_ERR_INVALID_ARG;
    if (s_player == NULL) return ESP_ERR_INVALID_STATE;

    esp_gmf_pipeline_handle_t pipeline = NULL;
    esp_gmf_io_handle_t input = NULL;
    if (esp_audio_simple_player_get_pipeline(s_player, &pipeline) != ESP_GMF_ERR_OK ||
        pipeline == NULL) return ESP_ERR_INVALID_STATE;
    if (esp_gmf_pipeline_get_in(pipeline, &input) != ESP_GMF_ERR_OK || input == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char *uri = NULL;
    if (esp_gmf_io_get_uri(input, &uri) != ESP_GMF_ERR_OK || uri == NULL ||
        strncmp(uri, "file://", 7) != 0) return ESP_ERR_NOT_SUPPORTED;

    uint64_t read_position = 0;
    uint64_t total_size = 0;
    if (esp_gmf_io_get_pos(input, &read_position) != ESP_GMF_ERR_OK ||
        esp_gmf_io_get_size(input, &total_size) != ESP_GMF_ERR_OK || total_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t buffered_bytes = 0;
    if (esp_gmf_io_get_db_filled_size(input, &buffered_bytes) != ESP_GMF_ERR_OK) {
        buffered_bytes = 0;
    }
    uint64_t consumed = read_position > buffered_bytes ? read_position - buffered_bytes : 0;
    if (consumed >= total_size) {
        *progress_permille = 1000;
    } else {
        *progress_permille = (uint32_t)((consumed * 1000u) / total_size);
    }
    return ESP_OK;
}

esp_err_t audio_radio_get_file_timing(uint32_t *elapsed_ms, uint32_t *duration_ms)
{
    if (elapsed_ms == NULL || duration_ms == NULL) return ESP_ERR_INVALID_ARG;

    uint64_t pcm_bytes = 0;
    uint32_t duration = 0;
    bool is_file = false;
    portENTER_CRITICAL(&s_timing_lock);
    pcm_bytes = s_file_pcm_bytes;
    duration = s_file_duration_ms;
    is_file = s_current_source_is_file;
    portEXIT_CRITICAL(&s_timing_lock);
    if (!is_file) return ESP_ERR_NOT_SUPPORTED;

    uint64_t bytes_per_second = (uint64_t)s_output_sample_rate *
                                s_output_channels * s_output_bits / 8;
    if (bytes_per_second == 0) return ESP_ERR_INVALID_STATE;
    uint64_t elapsed = pcm_bytes * 1000u / bytes_per_second;
    if (duration > 0 && elapsed > duration) elapsed = duration;
    *elapsed_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    *duration_ms = duration;
    return ESP_OK;
}

esp_err_t audio_radio_set_volume(int volume)
{
    ESP_RETURN_ON_FALSE(volume >= 0 && volume <= 100, ESP_ERR_INVALID_ARG,
                        TAG, "Volume must be in the range 0..100");
    const bool usb = s_active_output_usb;
    if (!usb && s_codec_dev != NULL && s_codec_opened) {
        ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_vol(s_codec_dev, volume)),
                            TAG, "Failed to set output volume");
    }
    s_output_volume = volume;
    if (usb) {
        s_usb_volume = volume;
        esp_err_t usb_ret = usb_audio_output_set_volume((uint8_t)volume);
        if (usb_ret != ESP_OK) {
            ESP_LOGW(TAG, "USB volume update failed: %s", esp_err_to_name(usb_ret));
        }
    } else {
        s_speaker_volume = volume;
    }
    audio_radio_save_volume(usb, volume);
    const int usb_gain_basis_points =
        volume * CONFIG_MYPAPER_USB_AUDIO_MAX_VOLUME;
    if (usb) {
        ESP_LOGI(TAG, "USB volume saved: %d (effective=%d.%02d%%, ceiling=%d%%)",
                 volume, usb_gain_basis_points / 100, usb_gain_basis_points % 100,
                 CONFIG_MYPAPER_USB_AUDIO_MAX_VOLUME);
    } else {
        ESP_LOGI(TAG, "Speaker volume saved: %d", volume);
    }
    return ESP_OK;
}

int audio_radio_get_volume(void)
{
    audio_radio_load_output_mode();
    return s_output_volume;
}

static esp_err_t audio_radio_retarget_running_pipeline(audio_output_mode_t mode,
                                                       uint32_t *target_rate)
{
    ESP_RETURN_ON_FALSE(target_rate != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Target-rate output is null");
    ESP_RETURN_ON_FALSE(s_player != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Player is not initialized");

    esp_gmf_pipeline_handle_t pipeline = NULL;
    esp_gmf_element_handle_t rate_el = NULL;
    esp_gmf_element_handle_t channel_el = NULL;
    esp_gmf_element_handle_t bit_el = NULL;
    ESP_RETURN_ON_FALSE(
        esp_audio_simple_player_get_pipeline(s_player, &pipeline) == ESP_GMF_ERR_OK &&
            pipeline != NULL &&
            esp_gmf_pipeline_get_el_by_name(pipeline, "aud_rate_cvt", &rate_el) ==
                ESP_GMF_ERR_OK &&
            rate_el != NULL &&
            esp_gmf_pipeline_get_el_by_name(pipeline, "aud_ch_cvt", &channel_el) ==
                ESP_GMF_ERR_OK && channel_el != NULL &&
            esp_gmf_pipeline_get_el_by_name(pipeline, "aud_bit_cvt", &bit_el) ==
                ESP_GMF_ERR_OK && bit_el != NULL,
        ESP_FAIL, TAG, "Failed to find PCM converters while switching output");

    if (mode != AUDIO_OUTPUT_SPEAKER) {
        ESP_RETURN_ON_ERROR(usb_audio_output_init(), TAG,
                            "Failed to initialize USB audio while switching output");
        const bool wait_for_usb = mode == AUDIO_OUTPUT_USB;
        if (wait_for_usb && !usb_audio_output_wait_connected(AUDIO_USB_CONNECT_WAIT_MS)) {
            return ESP_ERR_NOT_FOUND;
        }
        uint32_t usb_rate = 0;
        uint8_t usb_channels = 0;
        uint8_t usb_bits = 0;
        const uint32_t source_rate = s_source_format_valid ? s_source_sample_rate
                                                           : s_output_sample_rate;
        const uint8_t source_channels = s_source_format_valid ? s_source_channels
                                                               : s_output_channels;
        const uint8_t source_bits = s_source_format_valid ? s_source_bits : s_output_bits;
        if (usb_audio_output_select_format(source_rate, source_channels, source_bits, 0,
                                           &usb_rate, &usb_channels, &usb_bits)) {
            ESP_RETURN_ON_FALSE(
                esp_gmf_rate_cvt_clear_max_dest_rate(rate_el) == ESP_GMF_ERR_OK &&
                    esp_gmf_rate_cvt_set_dest_rate(rate_el, usb_rate) == ESP_GMF_ERR_OK &&
                    esp_gmf_ch_cvt_set_dest_channel(channel_el, usb_channels) ==
                        ESP_GMF_ERR_OK &&
                    esp_gmf_bit_cvt_set_dest_bits(bit_el, usb_bits) == ESP_GMF_ERR_OK,
                ESP_FAIL, TAG, "Failed to retarget running pipeline for USB");
            *target_rate = usb_rate;
            s_output_channels = usb_channels;
            s_output_bits = usb_bits;
            ESP_LOGI(TAG, "Running pipeline retargeted for USB: %" PRIu32
                     " Hz/%u ch/%u bit", usb_rate, usb_channels, usb_bits);
            return ESP_OK;
        }
        if (mode == AUDIO_OUTPUT_USB) return ESP_ERR_NOT_SUPPORTED;
    }

    // Speaker (or Auto without a headset) restores adaptive behavior. The
    // converter retains its decoded source rate and immediately recalculates
    // the correct destination, so 44.1 kHz returns to native playback.
    ESP_RETURN_ON_FALSE(
        esp_gmf_rate_cvt_set_max_dest_rate(rate_el, CONFIG_AUDIO_RADIO_SAMPLE_RATE) ==
            ESP_GMF_ERR_OK &&
            esp_gmf_rate_cvt_get_dest_rate(rate_el, target_rate) == ESP_GMF_ERR_OK &&
            esp_gmf_ch_cvt_set_dest_channel(channel_el, AUDIO_RADIO_CHANNELS) ==
                ESP_GMF_ERR_OK &&
            esp_gmf_bit_cvt_set_dest_bits(bit_el, AUDIO_RADIO_BITS) == ESP_GMF_ERR_OK,
        ESP_FAIL, TAG, "Failed to restore adaptive speaker rate");
    s_output_channels = AUDIO_RADIO_CHANNELS;
    s_output_bits = AUDIO_RADIO_BITS;
    return ESP_OK;
}

/**
 * @brief Point a live pipeline at the built-in speaker.
 *
 * Mirrors the speaker half of audio_radio_set_output_sample_rate(), minus the
 * output-mode policy: the pinned mode may still name a headset that is
 * physically gone, and moving a paused track to the speaker beats dropping it.
 * Assumes the caller holds the player paused, so the swap cannot race a write.
 */
static esp_err_t audio_radio_fall_back_to_speaker(uint32_t *target_rate)
{
    ESP_RETURN_ON_FALSE(target_rate != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Target-rate output is null");
    ESP_RETURN_ON_ERROR(
        audio_radio_retarget_running_pipeline(AUDIO_OUTPUT_SPEAKER, target_rate), TAG,
        "Failed to retarget the running pipeline for the speaker");
    (void)usb_audio_output_stop();
    s_active_output_usb = false;
    ESP_RETURN_ON_ERROR(audio_radio_configure_speaker(*target_rate), TAG,
                        "Failed to configure the speaker output");
    ESP_LOGI(TAG, "Active audio output: speaker (fallback, %" PRIu32 " Hz)", *target_rate);
    return ESP_OK;
}

esp_err_t audio_radio_set_output_mode(audio_output_mode_t mode)
{
    ESP_RETURN_ON_FALSE(mode >= AUDIO_OUTPUT_SPEAKER && mode <= AUDIO_OUTPUT_AUTO,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid audio output mode: %d", mode);
    audio_radio_load_output_mode();
    if (mode == s_output_mode) return ESP_OK;

    audio_output_mode_t previous_mode = s_output_mode;
    uint32_t previous_rate = s_output_sample_rate;
    bool was_playing = s_playback_state == AUDIO_PLAYBACK_PLAYING;
    if (was_playing) {
        ESP_RETURN_ON_ERROR(audio_radio_pause(), TAG,
                            "Failed to pause before changing audio output");
    }

    s_output_mode = mode;
    uint32_t target_rate = s_output_sample_rate;
    esp_err_t ret = was_playing
                        ? audio_radio_retarget_running_pipeline(mode, &target_rate)
                        : ESP_OK;
    if (ret == ESP_OK) {
        if (!was_playing && mode == AUDIO_OUTPUT_SPEAKER) {
            s_output_channels = AUDIO_RADIO_CHANNELS;
            s_output_bits = AUDIO_RADIO_BITS;
            if (target_rate > CONFIG_AUDIO_RADIO_SAMPLE_RATE) {
                target_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE;
            }
        }
        ret = audio_radio_set_output_sample_rate(target_rate);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio output switch to %s failed: %s",
                 audio_radio_get_output_mode_name(), esp_err_to_name(ret));
        s_output_mode = previous_mode;
        if (was_playing) {
            uint32_t rollback_rate = previous_rate;
            (void)audio_radio_retarget_running_pipeline(previous_mode, &rollback_rate);
            (void)audio_radio_set_output_sample_rate(rollback_rate);
        } else {
            (void)audio_radio_set_output_sample_rate(previous_rate);
        }
    } else {
        audio_radio_save_output_mode(mode);
    }

    if (was_playing) {
        esp_err_t resume_ret = audio_radio_resume();
        if (ret == ESP_OK && resume_ret != ESP_OK) ret = resume_ret;
    }
    return ret;
}

audio_output_mode_t audio_radio_get_output_mode(void)
{
    audio_radio_load_output_mode();
    return s_output_mode;
}

const char *audio_radio_get_output_mode_name(void)
{
    switch (audio_radio_get_output_mode()) {
        case AUDIO_OUTPUT_USB: return "USB";
        case AUDIO_OUTPUT_AUTO: return "AUTO";
        case AUDIO_OUTPUT_SPEAKER:
        default: return "SPK";
    }
}

const char *audio_radio_get_active_output_name(void)
{
    return s_active_output_usb ? "USB" : "SPK";
}

bool audio_radio_usb_connected(void)
{
    return usb_audio_output_is_connected();
}

void audio_radio_service_output(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_last_output_service_us != 0 &&
        now_us - s_last_output_service_us < AUDIO_OUTPUT_SERVICE_INTERVAL_US) {
        return;
    }
    s_last_output_service_us = now_us;

    bool connected = audio_radio_usb_connected();
    if (connected == s_last_usb_connected) return;
    s_last_usb_connected = connected;

    if (!connected) {
        if (!s_active_output_usb || s_playback_state != AUDIO_PLAYBACK_PLAYING) return;
        // The headset supplying the active output is gone. Pause instead of
        // tearing the player down: the sink write is no longer fatal to the
        // pipeline, so the decode position survives, and that is the only way to
        // keep it - esp_gmf_pipeline_seek does not support FLAC, which is most of
        // what the library holds. The user's next play moves the track to the
        // speaker; a replugged headset claims it back below.
        if (audio_radio_pause() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to pause playback after losing the USB output");
            return;
        }
        s_paused_after_usb_loss = true;
        return;
    }

    // A headset arrived. One the user pinned away from is left untouched; in Auto
    // and USB modes it is the preferred output.
    if (s_output_mode == AUDIO_OUTPUT_SPEAKER) return;

    if (s_paused_after_usb_loss) {
        // Claim back the track this function paused when the previous headset
        // left, so a replug resumes the music on its own and at its old position.
        // s_active_output_usb is still set but its stream died with the device, so
        // the sink has to be armed again rather than merely resumed into.
        if (s_playback_state != AUDIO_PLAYBACK_PAUSED) return;
        uint32_t target_rate = s_output_sample_rate;
        esp_err_t ret =
            audio_radio_retarget_running_pipeline(AUDIO_OUTPUT_AUTO, &target_rate);
        if (ret == ESP_OK) ret = audio_radio_set_output_sample_rate(target_rate);
        if (ret != ESP_OK) {
            // Stay paused rather than resume onto a sink that would swallow the
            // audio. The user still has a working play button.
            ESP_LOGW(TAG, "Failed to reclaim playback for the replugged headset: %s",
                     esp_err_to_name(ret));
            return;
        }
        (void)audio_radio_resume();
        return;
    }

    // A headset arrived mid-playback on the speaker. That switch is live, so
    // repoint the running pipeline rather than restarting the track.
    if (s_active_output_usb) return;
    bool was_playing = s_playback_state == AUDIO_PLAYBACK_PLAYING;
    if (!was_playing) return;
    if (audio_radio_pause() != ESP_OK) return;
    uint32_t target_rate = s_output_sample_rate;
    esp_err_t ret = audio_radio_retarget_running_pipeline(AUDIO_OUTPUT_AUTO, &target_rate);
    if (ret == ESP_OK) ret = audio_radio_set_output_sample_rate(target_rate);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Auto output reconciliation failed: %s", esp_err_to_name(ret));
    }
    (void)audio_radio_resume();
}

esp_err_t audio_radio_record_wav(const char *path, uint32_t duration_ms)
{
    char vfs_path[AUDIO_RADIO_RECORD_PATH_MAX];
    FILE *file = NULL;
    uint8_t *buffer = NULL;
    esp_err_t ret = ESP_OK;
    uint32_t written_size = 0;

    ESP_RETURN_ON_FALSE(duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "Record duration must be greater than 0");
    ESP_RETURN_ON_FALSE(!s_recording, ESP_ERR_INVALID_STATE, TAG, "Recording is already running");
    ESP_RETURN_ON_ERROR(audio_radio_normalize_record_path(path, vfs_path, sizeof(vfs_path)), TAG, "Invalid record path");
    ESP_RETURN_ON_ERROR(audio_radio_init(), TAG, "Audio radio init failed");
    ESP_RETURN_ON_ERROR(audio_radio_configure_speaker(CONFIG_AUDIO_RADIO_SAMPLE_RATE),
                        TAG, "Failed to configure recording sample rate");

    const uint32_t block_align = AUDIO_RADIO_CHANNELS * AUDIO_RADIO_BITS / 8;
    uint64_t target_size = ((uint64_t)CONFIG_AUDIO_RADIO_SAMPLE_RATE * duration_ms / 1000) * block_align;
    target_size -= target_size % block_align;
    ESP_RETURN_ON_FALSE(target_size > 0 && target_size <= (UINT32_MAX - 36),
                        ESP_ERR_INVALID_SIZE, TAG, "Record duration is too large");

    buffer = malloc(AUDIO_RADIO_RECORD_BUFFER_SIZE);
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate record buffer");

    file = fopen(vfs_path, "wb+");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open record file: %s", vfs_path);
        free(buffer);
        return ESP_FAIL;
    }

    s_recording = true;
    ESP_LOGI(TAG, "Recording WAV to %s, duration=%" PRIu32 " ms", vfs_path, duration_ms);

    ret = audio_radio_write_wav_header(file, 0);
    while (ret == ESP_OK && written_size < target_size) {
        uint32_t remain = (uint32_t)(target_size - written_size);
        int read_size = remain > AUDIO_RADIO_RECORD_BUFFER_SIZE ? AUDIO_RADIO_RECORD_BUFFER_SIZE : (int)remain;
        read_size -= read_size % block_align;
        if (read_size <= 0) {
            break;
        }

        ret = audio_radio_codec_ret_to_esp(esp_codec_dev_read(s_codec_dev, buffer, read_size));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read microphone data");
            break;
        }
        if (fwrite(buffer, 1, read_size, file) != (size_t)read_size) {
            ESP_LOGE(TAG, "Failed to write record file");
            ret = ESP_FAIL;
            break;
        }
        written_size += read_size;
    }

    if (ret == ESP_OK && fseek(file, 0, SEEK_SET) != 0) {
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK) {
        ret = audio_radio_write_wav_header(file, written_size);
    }
    fclose(file);
    free(buffer);
    s_recording = false;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Record complete: %s, %" PRIu32 " bytes PCM", vfs_path, written_size);
    }
    return ret;
}

void audio_radio_deinit(void)
{
    portENTER_CRITICAL(&s_timing_lock);
    ++s_duration_generation;
    portEXIT_CRITICAL(&s_timing_lock);
    const int64_t deinit_begin_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Deinit begin output=%s", s_active_output_usb ? "USB" : "SPK");
    if (s_player != NULL) {
        esp_err_t stop_ret = audio_radio_stop();
        ESP_LOGI(TAG, "Deinit: player stop returned %s", esp_err_to_name(stop_ret));
        const int64_t destroy_begin_us = esp_timer_get_time();
        int destroy_ret = esp_audio_simple_player_destroy(s_player);
        ESP_LOGI(TAG, "Deinit: player destroy ret=%d elapsed=%lld ms", destroy_ret,
                 (long long)((esp_timer_get_time() - destroy_begin_us) / 1000));
        s_player = NULL;
    }
    s_playback_active = false;
    s_playback_ready = false;
    s_playback_state = AUDIO_PLAYBACK_IDLE;
    s_hls_last_format = 0;
    s_output_sample_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE;
    s_codec_sample_rate = 0;
    s_output_format_ok = true;
    s_output_write_gated = true;
    s_active_output_usb = false;
    s_last_usb_connected = false;
    s_last_output_service_us = 0;
    // Per-session. The next activity in is a fresh playback context and must not
    // inherit a pause that was only ever about the output the last one used.
    s_paused_after_usb_loss = false;
    portENTER_CRITICAL(&s_timing_lock);
    s_file_pcm_bytes = 0;
    s_file_duration_ms = 0;
    s_current_source_is_file = false;
    portEXIT_CRITICAL(&s_timing_lock);
    s_hls_io = NULL;
    const int64_t usb_deinit_begin_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Deinit: USB host begin");
    usb_audio_output_deinit();
    ESP_LOGI(TAG, "Deinit: USB host complete elapsed=%lld ms",
             (long long)((esp_timer_get_time() - usb_deinit_begin_us) / 1000));

    if (s_codec_dev != NULL) {
        if (s_codec_opened) {
            esp_codec_dev_set_out_mute(s_codec_dev, true);
            audio_radio_set_pa(false);
            esp_codec_dev_close(s_codec_dev);
            s_codec_opened = false;
        }
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    } else {
        audio_radio_set_pa(false);
    }

    if (s_codec_if != NULL) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_data_if != NULL) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if != NULL) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }

    if (s_i2s_tx != NULL) {
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_i2s_rx != NULL) {
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }
    ESP_LOGI(TAG, "Deinit complete total=%lld ms",
             (long long)((esp_timer_get_time() - deinit_begin_us) / 1000));
}

void *audio_radio_get_player(void)
{
    return s_player;
}
