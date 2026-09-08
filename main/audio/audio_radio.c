#include "audio_radio.h"

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
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_extractor.h"
#include "esp_extractor_defaults.h"
#include "esp_fourcc.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_hls_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "media_lib_adapter.h"

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
static uint32_t s_output_sample_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE;
static volatile bool s_output_format_ok = true;
static portMUX_TYPE s_timing_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_file_pcm_bytes;
static uint32_t s_file_duration_ms;
static volatile bool s_current_source_is_file;
static bool s_extractor_registered;

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
        }
    }
    if (extractor != NULL) esp_extractor_close(extractor);

    bool used_flac_streaminfo = false;
    if ((extractor_ret != ESP_EXTRACTOR_ERR_OK || *duration_ms == 0) &&
        extractor_type == ESP_EXTRACTOR_TYPE_FLAC) {
        used_flac_streaminfo = audio_radio_read_flac_streaminfo_duration(file, duration_ms);
    }
    fclose(file);

    if (!used_flac_streaminfo &&
        (extractor_ret != ESP_EXTRACTOR_ERR_OK || *duration_ms == 0)) {
        ESP_LOGW(TAG, "Audio duration unavailable for %s, extractor ret=%d",
                 path, extractor_ret);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "Audio metadata duration=%" PRIu32 " ms source=%s path=%s",
             *duration_ms, used_flac_streaminfo ? "flac-streaminfo" : "extractor", path);
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

static int audio_radio_write_cb(uint8_t *pcm, int len, void *ctx)
{
    esp_codec_dev_handle_t codec_dev = (esp_codec_dev_handle_t) ctx;
    if (pcm == NULL || len <= 0 || codec_dev == NULL || !s_output_format_ok) {
        return -1;
    }
    int ret = esp_codec_dev_write(codec_dev, pcm, (int) len);
    if (ret == ESP_CODEC_DEV_OK && s_current_source_is_file) {
        portENTER_CRITICAL(&s_timing_lock);
        s_file_pcm_bytes += (uint32_t)len;
        portEXIT_CRITICAL(&s_timing_lock);
    }
    return ret == ESP_CODEC_DEV_OK ? len : -1;
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
        esp_err_t ret = audio_radio_set_output_sample_rate((uint32_t)info->sample_rate);
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

static esp_err_t audio_radio_set_output_sample_rate(uint32_t sample_rate)
{
    ESP_RETURN_ON_FALSE(sample_rate > 0 && sample_rate <= CONFIG_AUDIO_RADIO_SAMPLE_RATE,
                        ESP_ERR_INVALID_ARG, TAG, "Unsupported output sample rate: %" PRIu32, sample_rate);
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "Codec is not initialized");
    if (s_codec_opened && sample_rate == s_output_sample_rate) {
        s_output_format_ok = true;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Reconfiguring I2S/ES8311 output: %" PRIu32 " -> %" PRIu32 " Hz",
             s_output_sample_rate, sample_rate);
    if (s_codec_opened) {
        esp_codec_dev_set_out_mute(s_codec_dev, true);
        audio_radio_set_pa(false);
        ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_close(s_codec_dev)),
                            TAG, "Failed to close codec for sample-rate change");
        s_codec_opened = false;
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
    s_output_sample_rate = sample_rate;
    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_vol(s_codec_dev, s_output_volume)),
                        TAG, "Failed to restore output volume");
    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_mute(s_codec_dev, false)),
                        TAG, "Failed to unmute reconfigured output");
    ESP_RETURN_ON_ERROR(audio_radio_set_pa(true), TAG, "Failed to enable PA after sample-rate change");
    s_output_format_ok = true;
    return ESP_OK;
}

static int audio_radio_prepare_player_cb(esp_asp_handle_t *handle, void *ctx)
{
    (void)ctx;
    esp_gmf_pipeline_handle_t pipeline = NULL;
    esp_gmf_element_handle_t rate_el = NULL;
    if (esp_audio_simple_player_get_pipeline((esp_asp_handle_t)handle, &pipeline) != ESP_GMF_ERR_OK ||
        pipeline == NULL ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_rate_cvt", &rate_el) != ESP_GMF_ERR_OK ||
        rate_el == NULL) {
        ESP_LOGE(TAG, "Failed to find rate converter before playback");
        return -1;
    }
    if (esp_gmf_rate_cvt_set_max_dest_rate(rate_el, CONFIG_AUDIO_RADIO_SAMPLE_RATE) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to set adaptive sample-rate cap");
        return -1;
    }
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
            .user_ctx = s_codec_dev,
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
                // Keep the HTTP producer (priority 10) ahead of the HLS
                // consumer during segment reload so the old HTTP job can
                // finish cleanup before HLS registers the next one.
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
    ESP_RETURN_ON_ERROR(audio_radio_init_i2s(), TAG, "I2S init failed");
    ESP_RETURN_ON_ERROR(audio_radio_init_codec(), TAG, "Codec init failed");
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
    s_playback_ready = false;
    portENTER_CRITICAL(&s_timing_lock);
    s_file_pcm_bytes = 0;
    s_file_duration_ms = 0;
    s_current_source_is_file = strncmp(url, "file://", 7) == 0;
    portEXIT_CRITICAL(&s_timing_lock);
    s_output_format_ok = true;
    int run_ret = esp_audio_simple_player_run(s_player, url, NULL);
    if (run_ret != ESP_GMF_ERR_OK) {
        s_current_source_is_file = false;
        s_playback_ready = false;
        s_playback_active = false;
        s_playback_state = AUDIO_PLAYBACK_ERROR;
        ESP_LOGE(TAG, "Failed to start audio playback: %d", run_ret);
        return ESP_FAIL;
    }
    s_playback_active = true;
    s_playback_state = AUDIO_PLAYBACK_PLAYING;
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

    ESP_LOGI(TAG, "Stopping current radio stream before switching");
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_stop(s_player) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Failed to stop playback");
    s_playback_active = false;
    s_playback_ready = false;
    s_playback_state = AUDIO_PLAYBACK_STOPPED;
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
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_resume(s_player) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Failed to resume playback");
    s_playback_active = true;
    s_playback_state = AUDIO_PLAYBACK_PLAYING;
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
                                AUDIO_RADIO_CHANNELS * AUDIO_RADIO_BITS / 8;
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
    if (s_codec_dev != NULL && s_codec_opened) {
        ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_vol(s_codec_dev, volume)),
                            TAG, "Failed to set output volume");
    }
    s_output_volume = volume;
    ESP_LOGI(TAG, "Output volume: %d", s_output_volume);
    return ESP_OK;
}

int audio_radio_get_volume(void)
{
    return s_output_volume;
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
    ESP_RETURN_ON_ERROR(audio_radio_set_output_sample_rate(CONFIG_AUDIO_RADIO_SAMPLE_RATE),
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
    if (s_player != NULL) {
        audio_radio_stop();
        esp_audio_simple_player_destroy(s_player);
        s_player = NULL;
    }
    s_playback_active = false;
    s_playback_ready = false;
    s_playback_state = AUDIO_PLAYBACK_IDLE;
    s_hls_last_format = 0;
    s_output_sample_rate = CONFIG_AUDIO_RADIO_SAMPLE_RATE;
    s_output_format_ok = true;
    portENTER_CRITICAL(&s_timing_lock);
    s_file_pcm_bytes = 0;
    s_file_duration_ms = 0;
    s_current_source_is_file = false;
    portEXIT_CRITICAL(&s_timing_lock);
    s_hls_io = NULL;

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

}

void *audio_radio_get_player(void)
{
    return s_player;
}
