#include "audio_radio.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "sdkconfig.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_fourcc.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
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

#define AUDIO_RADIO_I2C_PORT      ((i2c_port_t)CONFIG_AUDIO_RADIO_I2C_PORT)
#define AUDIO_RADIO_I2S_PORT      I2S_NUM_0
#define AUDIO_RADIO_BITS          16
#define AUDIO_RADIO_CHANNELS      2
#define AUDIO_RADIO_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_256
#define AUDIO_RADIO_HLS_BUFFER_SIZE (128 * 1024)
#define AUDIO_RADIO_HLS_TASK_STACK  (20 * 1024)
#define AUDIO_RADIO_RECORD_BUFFER_SIZE 4096
#define AUDIO_RADIO_RECORD_PATH_MAX    256

static const char *TAG = "audio_radio";

static i2c_master_bus_handle_t s_i2c_bus;
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

static int audio_radio_write_cb(uint8_t *pcm, int len, void *ctx)
{
    esp_codec_dev_handle_t codec_dev = (esp_codec_dev_handle_t) ctx;
    if (pcm == NULL || len <= 0 || codec_dev == NULL) {
        return -1;
    }
    int ret = esp_codec_dev_write(codec_dev, pcm, (int) len);
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
    } else if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO && pkt->payload != NULL) {
        esp_asp_music_info_t *info = (esp_asp_music_info_t *)pkt->payload;
        ESP_LOGI(TAG, "Music info: %d Hz, %d ch, %d bit, bitrate=%d",
                 (int)info->sample_rate,
                 (int)info->channels,
                 (int)info->bits,
                 (int)info->bitrate);
    }

    return 0;
}

static int audio_radio_hls_media_type_cb(esp_hls_file_seg_info_t *info, void *ctx)
{
    if (info == NULL || ctx == NULL) {
        return -1;
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

static esp_err_t audio_radio_init_i2c(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = AUDIO_RADIO_I2C_PORT,
        .sda_io_num = CONFIG_AUDIO_RADIO_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_AUDIO_RADIO_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "Failed to create I2C bus");
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

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_RADIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_FAIL, TAG, "Failed to create codec I2C control interface");

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

    ESP_RETURN_ON_ERROR(audio_radio_codec_ret_to_esp(esp_codec_dev_set_out_vol(s_codec_dev, CONFIG_AUDIO_RADIO_OUTPUT_VOLUME)),
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
                .prio = 20,
                .core = 1,
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
    return ESP_OK;
}

esp_err_t audio_radio_init(void)
{
    ESP_RETURN_ON_ERROR(audio_radio_init_i2c(), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(audio_radio_init_i2s(), TAG, "I2S init failed");
    ESP_RETURN_ON_ERROR(audio_radio_init_codec(), TAG, "Codec init failed");
    ESP_RETURN_ON_ERROR(audio_radio_init_player(), TAG, "Player init failed");
    return ESP_OK;
}

esp_err_t audio_radio_play(const char *url)
{
    ESP_RETURN_ON_FALSE(url != NULL && url[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "Radio URL is empty");
    ESP_RETURN_ON_ERROR(audio_radio_init(), TAG, "Audio radio init failed");

    ESP_LOGI(TAG, "Playing radio URL: %s", url);
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_run(s_player, url, NULL) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "Failed to start radio playback");
    return ESP_OK;
}

esp_err_t audio_radio_play_default(void)
{
    ESP_LOGI(TAG, "Default station: %s", CONFIG_AUDIO_RADIO_NAME);
    return audio_radio_play(CONFIG_AUDIO_RADIO_URL);
}

esp_err_t audio_radio_stop(void)
{
    if (s_player == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_stop(s_player) == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "Failed to stop playback");
    return ESP_OK;
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
        esp_audio_simple_player_stop(s_player);
        esp_audio_simple_player_destroy(s_player);
        s_player = NULL;
    }
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

    if (s_i2s_tx != NULL) {
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_i2s_rx != NULL) {
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }

    if (s_i2c_bus != NULL) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }

    s_codec_if = NULL;
    s_gpio_if = NULL;
    s_ctrl_if = NULL;
    s_data_if = NULL;
}

void *audio_radio_get_player(void)
{
    return s_player;
}
