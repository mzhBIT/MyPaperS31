// MyPaperS31 speaker backend.
//
// The board uses a BH1098G Bluetooth-audio SoC as the I2S *master* and DAC,
// followed by an NS4150B amplifier whose enable (PA_EN, P0.4) sits on the
// PCA9535 expander.  The ESP32-S31 therefore cannot choose the sample rate:
// it must follow the BCLK/WS the BH1098G generates.  We measure that rate from
// the I2S RX side and resample every source to it before writing I2S.
//
// The reference (Metalio E-INK4-Plus) selects BT mode 1 ("conversation") with
// AT+RX=2 / AT+MODE=1 on UART1 at 115200, then streams 32-bit stereo slots
// whose upper 16 bits carry signed 16-bit PCM.
//
// Local music uses the simple decoder; radio reuses the P4 HTTP/HLS GMF layer.
// Both feed this board's BH1098G sink through the public audio_radio.h API.

#include "audio/audio_radio.h"
#include "audio/file_duration.h"
#include "boards/MyPaperS31/Pins.h"

#include "esp_ae_bit_cvt.h"
#include "esp_ae_ch_cvt.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_hls_io.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_uri_parser.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "media_lib_adapter.h"

#include <M5Unified.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <strings.h>

namespace pins = mypaper::board::s31;

namespace {

constexpr const char *TAG = "AUDIO_S31";

constexpr const char *NVS_NAMESPACE = "audio_out";
constexpr const char *NVS_MODE_KEY = "mode";
constexpr const char *NVS_SPK_VOLUME_KEY = "spk_volume";

constexpr uint32_t DEFAULT_OUTPUT_RATE = 16000;
constexpr size_t INPUT_CHUNK_BYTES = 8192;
constexpr size_t OUTPUT_CHUNK_INITIAL = 16384;
constexpr size_t OUTPUT_CHUNK_MAX = 512 * 1024;
constexpr size_t WRITE_CHUNK_FRAMES = 512;
constexpr uint32_t WORKER_STACK_BYTES = 12288;
constexpr UBaseType_t WORKER_PRIORITY = 5;
// The BT/PA rail is switched on when the backend initializes, so the module
// still has to boot before it accepts AT commands.  200 ms was demonstrably
// too short (AT+RX=2 went unanswered); the reference board leaves the rail on
// for seconds before the same sequence.
constexpr uint32_t BT_BOOT_SETTLE_MS = 1200;


// PCA9535 register map.  P0.4 is PA_EN (NS4150B CTRL, active high) and
// P1.4 is the BT + PA power rail (active high).
constexpr uint8_t PCA_PORT0_OUTPUT = 0x02;
constexpr uint8_t PCA_PORT0_CONFIG = 0x06;
constexpr uint8_t PCA_PORT1_OUTPUT = 0x03;
constexpr uint8_t PCA_PORT1_CONFIG = 0x07;
constexpr uint8_t PCA_PA_MASK = 1u << (pins::pa_enable_io & 7);
constexpr uint8_t PCA_BT_POWER_MASK = 1u << (pins::bt_power_io & 7);

constexpr unsigned CODEC_DMA_DESC_NUM = 6;
constexpr unsigned CODEC_DMA_FRAME_NUM = 240;

std::atomic<bool> s_initialized{false};
std::atomic<int> s_state{AUDIO_PLAYBACK_IDLE};
std::atomic<int> s_volume{30};
std::atomic<int> s_output_mode{AUDIO_OUTPUT_SPEAKER};
std::atomic<bool> s_stop{false};
std::atomic<bool> s_pause{false};
std::atomic<bool> s_bt_running{false};
std::atomic<bool> s_bt_ready{false};
std::atomic<uint32_t> s_output_rate{DEFAULT_OUTPUT_RATE};
std::atomic<uint64_t> s_played_frames{0};
std::atomic<uint32_t> s_file_pos{0};
std::atomic<uint32_t> s_file_size{0};
std::atomic<uint32_t> s_duration_ms{0};
std::atomic<uint32_t> s_duration_generation{0};
QueueHandle_t s_duration_queue = nullptr;
esp_asp_handle_t s_network_player = nullptr; // Protected by s_lock.
std::atomic<bool> s_network_active{false};
std::atomic<bool> s_network_write_enabled{false};
uint32_t s_hls_format = 0;
wifi_ps_type_t s_network_previous_ps = WIFI_PS_NONE;
bool s_network_restore_ps = false; // Protected by s_lock.
std::atomic<bool> s_network_endpoint_logged{false};

i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;
TaskHandle_t s_worker = nullptr;
SemaphoreHandle_t s_lock = nullptr;
SemaphoreHandle_t s_exited = nullptr;
SemaphoreHandle_t s_bt_done = nullptr;
bool s_uart_installed = false;
bool s_decoders_registered = false;

void ensure_sync()
{
    if (s_lock == nullptr) s_lock = xSemaphoreCreateMutex();
    if (s_exited == nullptr) s_exited = xSemaphoreCreateBinary();
    if (s_bt_done == nullptr) s_bt_done = xSemaphoreCreateBinary();
}

// ---------------------------------------------------------------- PCA9535 --

bool pca_read(uint8_t reg, uint8_t *value)
{
    return m5::In_I2C.readRegister(pins::io_address, reg, value, 1, 100000);
}

bool pca_write(uint8_t reg, uint8_t value)
{
    return m5::In_I2C.writeRegister8(pins::io_address, reg, value, 100000);
}

// Latch the requested level, then turn the pin into an output.  Writing the
// latch first avoids a glitch while the pin is still high-Z.
bool pca_output_bit(uint8_t output_reg, uint8_t config_reg, uint8_t mask, bool high)
{
    uint8_t output = 0;
    uint8_t config = 0;
    if (!pca_read(output_reg, &output) || !pca_read(config_reg, &config)) {
        ESP_LOGE(TAG, "PCA9535 read failed reg=0x%02x/0x%02x", output_reg, config_reg);
        return false;
    }
    if (high) output |= mask;
    else output &= (uint8_t)~mask;
    if (!pca_write(output_reg, output)) {
        ESP_LOGE(TAG, "PCA9535 output write failed reg=0x%02x", output_reg);
        return false;
    }
    config &= (uint8_t)~mask;
    if (!pca_write(config_reg, config)) {
        ESP_LOGE(TAG, "PCA9535 config write failed reg=0x%02x", config_reg);
        return false;
    }
    return true;
}

void pa_set(bool on)
{
    const bool ok = on ? m5::In_I2C.bitOn(pins::io_address, PCA_PORT0_OUTPUT,
                                          PCA_PA_MASK, 100000)
                       : m5::In_I2C.bitOff(pins::io_address, PCA_PORT0_OUTPUT,
                                           PCA_PA_MASK, 100000);
    if (ok) {
        ESP_LOGI(TAG, "PA_EN %s (PCA P0.4)", on ? "on" : "off");
    } else {
        ESP_LOGW(TAG, "PA_EN write failed (on=%d)", on);
    }
}

// -------------------------------------------------------------- converters --

struct Converters {
    esp_ae_bit_cvt_handle_t bit = nullptr;
    esp_ae_ch_cvt_handle_t channel = nullptr;
    esp_ae_rate_cvt_handle_t rate = nullptr;
    uint32_t src_rate = 0;
    uint32_t dst_rate = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    bool configured = false;
    uint8_t *stage1 = nullptr;
    uint8_t *stage2 = nullptr;
    uint8_t *stage3 = nullptr;
    size_t cap1 = 0;
    size_t cap2 = 0;
    size_t cap3 = 0;

    void close()
    {
        if (bit) esp_ae_bit_cvt_close(bit);
        if (channel) esp_ae_ch_cvt_close(channel);
        if (rate) esp_ae_rate_cvt_close(rate);
        bit = nullptr;
        channel = nullptr;
        rate = nullptr;
        free(stage1);
        free(stage2);
        free(stage3);
        stage1 = stage2 = stage3 = nullptr;
        cap1 = cap2 = cap3 = 0;
        src_rate = dst_rate = 0;
        channels = 0;
        bits = 0;
        configured = false;
    }

    static bool ensure(uint8_t **buffer, size_t *capacity, size_t need)
    {
        if (*capacity >= need) return true;
        uint8_t *next = static_cast<uint8_t *>(realloc(*buffer, need));
        if (next == nullptr) return false;
        *buffer = next;
        *capacity = need;
        return true;
    }

    bool configure(uint32_t source_rate, uint8_t source_channels, uint8_t source_bits,
                   uint32_t target_rate)
    {
        if (configured && source_rate == src_rate && target_rate == dst_rate &&
            source_channels == channels && source_bits == bits) {
            return true;
        }
        close();
        src_rate = source_rate;
        dst_rate = target_rate;
        channels = source_channels;
        bits = source_bits;
        configured = true;

        if (source_bits != 16) {
            esp_ae_bit_cvt_cfg_t cfg = {
                .sample_rate = source_rate,
                .channel = source_channels,
                .src_bits = source_bits,
                .dest_bits = 16,
            };
            if (esp_ae_bit_cvt_open(&cfg, &bit) != ESP_AE_ERR_OK) {
                ESP_LOGE(TAG, "bit converter open failed (%u bit)", source_bits);
                return false;
            }
        }
        if (source_channels != 2) {
            esp_ae_ch_cvt_cfg_t cfg = {
                .sample_rate = source_rate,
                .bits_per_sample = 16,
                .src_ch = source_channels,
                .dest_ch = 2,
                .weight = nullptr,
                .weight_len = 0,
            };
            if (esp_ae_ch_cvt_open(&cfg, &channel) != ESP_AE_ERR_OK) {
                ESP_LOGE(TAG, "channel converter open failed (%u ch)", source_channels);
                return false;
            }
        }
        if (source_rate != target_rate) {
            esp_ae_rate_cvt_cfg_t cfg = {
                .src_rate = source_rate,
                .dest_rate = target_rate,
                .channel = 2,
                .bits_per_sample = 16,
                .complexity = 2,
                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY,
            };
            if (esp_ae_rate_cvt_open(&cfg, &rate) != ESP_AE_ERR_OK) {
                ESP_LOGE(TAG, "rate converter open failed %u -> %u",
                         source_rate, target_rate);
                return false;
            }
        }
        ESP_LOGI(TAG, "PCM %u Hz %u ch %u bit -> %u Hz 2 ch 16 bit",
                 source_rate, source_channels, source_bits, target_rate);
        return true;
    }

    // Converts interleaved PCM to 16-bit stereo at dst_rate.
    bool process(const uint8_t *input, size_t length, const uint8_t **output,
                 uint32_t *frames)
    {
        if (channels == 0 || bits < 8) return false;
        const size_t bytes_per_frame = (size_t)channels * (bits / 8);
        if (bytes_per_frame == 0) return false;
        uint32_t count = (uint32_t)(length / bytes_per_frame);
        if (count == 0) return false;

        const uint8_t *data = input;
        if (bit != nullptr) {
            if (!ensure(&stage1, &cap1, (size_t)count * channels * 2)) return false;
            if (esp_ae_bit_cvt_process(bit, count, (esp_ae_sample_t)data, stage1) !=
                ESP_AE_ERR_OK) {
                return false;
            }
            data = stage1;
        }
        if (channel != nullptr) {
            if (!ensure(&stage2, &cap2, (size_t)count * 2 * 2)) return false;
            if (esp_ae_ch_cvt_process(channel, count, (esp_ae_sample_t)data, stage2) !=
                ESP_AE_ERR_OK) {
                return false;
            }
            data = stage2;
        }
        if (rate != nullptr) {
            uint32_t max_out = 0;
            if (esp_ae_rate_cvt_get_max_out_sample_num(rate, count, &max_out) !=
                    ESP_AE_ERR_OK ||
                max_out == 0) {
                return false;
            }
            if (!ensure(&stage3, &cap3, (size_t)max_out * 2 * 2)) return false;
            uint32_t out_count = max_out;
            if (esp_ae_rate_cvt_process(rate, (esp_ae_sample_t)data, count,
                                        stage3, &out_count) != ESP_AE_ERR_OK) {
                return false;
            }
            data = stage3;
            count = out_count;
        }
        *output = data;
        *frames = count;
        return true;
    }
};

// ------------------------------------------------------------------- sink ---

int volume_factor()
{
    const int volume = s_volume.load();
    return volume * volume * 65536 / 10000;
}

bool i2s_write_all(const void *data, size_t bytes)
{
    // The backend may have failed to set up I2S; playback then reports
    // failure instead of dereferencing a null channel.
    if (s_tx == nullptr) return false;
    const uint8_t *cursor = static_cast<const uint8_t *>(data);
    int64_t wait_start = 0;
    unsigned timeouts = 0;
    while (bytes > 0) {
        if (s_stop.load()) return false;
        size_t written = 0;
        // I2S takes milliseconds, unlike the UART/FreeRTOS APIs below.
        const esp_err_t err = i2s_channel_write(s_tx, cursor, bytes, &written,
                                                100);
        // A timed-out call can still have copied part of the PCM into DMA.
        // Retrying that prefix would duplicate samples and prevent progress.
        cursor += written;
        bytes -= written;
        if (err == ESP_OK) {
            if (written == 0) return false;
            if (wait_start != 0) {
                ESP_LOGW(TAG, "I2S clock acquired after %lld ms (%u timeouts)",
                         (long long)((esp_timer_get_time() - wait_start) / 1000),
                         timeouts);
                wait_start = 0;
                timeouts = 0;
            }
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
            return false;
        } else {
            // Lack of DMA progress can indicate missing clocks, but a timeout
            // alone does not prove that BCLK/WS have stopped.
            if (wait_start == 0) wait_start = esp_timer_get_time();
            ++timeouts;
            if (timeouts == 1 || timeouts % 10 == 0) {
                ESP_LOGW(TAG, "I2S write timeout %u: copied=%u remaining=%u",
                         timeouts, (unsigned)written, (unsigned)bytes);
            }
        }
    }
    return true;
}

// Expand signed 16-bit stereo to the BH1098G layout: left-justified 16-bit
// samples in 32-bit slots, duplicated to both channels, with a square-law
// software volume (100 == full scale).
bool write_pcm(const uint8_t *data, uint32_t frames)
{
    int32_t expanded[WRITE_CHUNK_FRAMES * 2];
    const auto *samples = reinterpret_cast<const int16_t *>(data);
    const int factor = volume_factor();

    while (frames > 0 && !s_stop.load()) {
        const uint32_t chunk = frames < WRITE_CHUNK_FRAMES ? frames : WRITE_CHUNK_FRAMES;
        for (uint32_t i = 0; i < chunk; ++i) {
            int32_t value = 0;
            if (factor != 0) {
                const int64_t scaled = (int64_t)samples[i * 2] * factor;
                value = scaled > INT32_MAX ? INT32_MAX
                        : scaled < INT32_MIN ? INT32_MIN
                                             : (int32_t)scaled;
            }
            expanded[i * 2] = value;
            expanded[i * 2 + 1] = value;
        }
        if (!i2s_write_all(expanded, (size_t)chunk * 2 * sizeof(int32_t))) return false;
        samples += chunk * 2;
        frames -= chunk;
        s_played_frames.fetch_add(chunk);
    }
    return true;
}

void destroy_i2s();

bool create_i2s(bool esp_master)
{
    i2s_chan_config_t channel_config = {
        .id = I2S_NUM_0,
        .role = esp_master ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE,
        .dma_desc_num = CODEC_DMA_DESC_NUM,
        .dma_frame_num = CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
        .tx_destination = I2S_DESTINATION_DMA,
        .rx_destination = I2S_DESTINATION_DMA,
    };
    esp_err_t err = i2s_new_channel(&channel_config, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel creation failed: %s", esp_err_to_name(err));
        return false;
    }

    // BH1098G supplies BCLK/WS; 32-bit stereo slots carry left-justified PCM.
    i2s_std_config_t std_config = {};
    std_config.clk_cfg.sample_rate_hz = DEFAULT_OUTPUT_RATE;
    std_config.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
#if SOC_I2S_HW_VERSION_2
    std_config.clk_cfg.ext_clk_freq_hz = 0;
#endif
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_config.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_config.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_config.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_config.slot_cfg.ws_pol = false;
    std_config.slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_2
    std_config.slot_cfg.left_align = true;
    std_config.slot_cfg.big_endian = false;
    std_config.slot_cfg.bit_order_lsb = false;
#endif
    std_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_config.gpio_cfg.bclk = (gpio_num_t)pins::i2s_bclk;
    std_config.gpio_cfg.ws = (gpio_num_t)pins::i2s_ws;
    std_config.gpio_cfg.dout = (gpio_num_t)pins::i2s_dout;
    std_config.gpio_cfg.din = (gpio_num_t)pins::i2s_din;

    err = i2s_channel_init_std_mode(s_tx, &std_config);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &std_config);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        destroy_i2s();
        return false;
    }
    ESP_LOGI(TAG, "I2S %s ready bclk=%d ws=%d dout=%d din=%d (32-bit stereo slots)",
             esp_master ? "master 16000 Hz" : "slave",
             pins::i2s_bclk, pins::i2s_ws, pins::i2s_dout, pins::i2s_din);
    return true;
}

void destroy_i2s()
{
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
    }
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = nullptr;
    }
}

// Sample a clock line against both internal pulls.  A line the module drives
// wins over the weak pull; a high-Z line follows it.  Only a line that reads
// low with pull-down AND high with pull-up is treated as idle, so a toggling
// or noisy signal is never mistaken for high-Z.
bool probe_pin_driven(gpio_num_t pin, const char *name)
{
    auto sample = [pin](bool pull_up) {
        gpio_config_t config = {};
        config.pin_bit_mask = 1ULL << pin;
        config.mode = GPIO_MODE_INPUT;
        config.pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        config.pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&config);
        esp_rom_delay_us(200);
        unsigned high = 0;
        for (int i = 0; i < 64; ++i) {
            high += gpio_get_level(pin) ? 1 : 0;
            esp_rom_delay_us(10);
        }
        return high;
    };
    const unsigned high_pull_down = sample(false);
    const unsigned high_pull_up = sample(true);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    const bool driven = high_pull_down > 0 || high_pull_up < 64;
    ESP_LOGI(TAG, "I2S %s GPIO%d %s (pull-down high=%u/64, pull-up high=%u/64)",
             name, (int)pin, driven ? "driven by BH1098G" : "high-Z",
             high_pull_down, high_pull_up);
    return driven;
}


bool start_uart()
{
    uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .rx_glitch_filt_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };
    esp_err_t err = uart_driver_install(UART_NUM_1, 1024, 0, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "BT UART install failed: %s", esp_err_to_name(err));
        return false;
    }
    s_uart_installed = (err == ESP_OK);
    if (s_uart_installed) {
        err = uart_param_config(UART_NUM_1, &config);
        if (err == ESP_OK) {
            err = uart_set_pin(UART_NUM_1, (int)pins::bt_tx, (int)pins::bt_rx,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BT UART config failed: %s", esp_err_to_name(err));
            uart_driver_delete(UART_NUM_1);
            s_uart_installed = false;
            return false;
        }
    }
    ESP_LOGI(TAG, "BT UART ready TX=%d RX=%d 115200", pins::bt_tx, pins::bt_rx);
    return true;
}

void uart_send(const char *command)
{
    if (!s_uart_installed) return;
    ESP_LOGI(TAG, "BT TX: %s", command);
    uart_write_bytes(UART_NUM_1, command, strlen(command));
    uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
}

bool uart_log_reply(uint32_t timeout_ms)
{
    if (!s_uart_installed) return false;
    uint8_t reply[128];
    const int size = uart_read_bytes(UART_NUM_1, reply, sizeof(reply) - 1,
                                     pdMS_TO_TICKS(timeout_ms));
    if (size <= 0) {
        ESP_LOGW(TAG, "BT RX: no reply within %u ms", (unsigned)timeout_ms);
        return false;
    }
    for (int i = 0; i < size; ++i) {
        if (reply[i] < 0x20 || reply[i] >= 0x7f) reply[i] = ' ';
    }
    reply[size] = 0;
    ESP_LOGI(TAG, "BT RX (%d): %s", size, (const char *)reply);
    return true;
}

uint32_t snap_rate(uint32_t measured)
{
    static constexpr uint32_t candidates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
    };
    uint32_t best = 0;
    uint32_t best_delta = UINT32_MAX;
    for (uint32_t candidate : candidates) {
        const uint32_t delta = measured > candidate ? measured - candidate
                                                    : candidate - measured;
        if (delta < best_delta) {
            best_delta = delta;
            best = candidate;
        }
    }
    // 5% covers a cheap master oscillator while rejecting the bogus rates a
    // short or gated measurement produces (10-14 kHz for a 16 kHz clock).
    if (best == 0 || best_delta > best / 20) return 0;
    return best;
}

// Count I2S RX bytes over a short window to discover the BH1098G's actual
// master clock.  The data itself is irrelevant (the pin may even float when
// there is no microphone); only BCLK/WS matter.
uint32_t measure_master_rate()
{
    if (s_rx == nullptr) return 0;
    // Do not start the clock before the first read: i2s_channel_read returns
    // only once a whole DMA chunk has arrived, and the BH1098G needs a few
    // milliseconds to spin up after AT+MODE=1.  Timing from the call would
    // count that warm-up as missing data and report a rate ~10% too low.
    // Chunk N carries exactly the data that arrived since chunk N-1 was
    // completed, so measure between completions and skip the first chunk.
    uint8_t sample[1024];
    uint64_t bytes = 0;
    int64_t first = 0;
    int64_t last = 0;
    unsigned chunks = 0;
    unsigned timeouts = 0;
    const int64_t deadline = esp_timer_get_time() + 800000;
    while (esp_timer_get_time() < deadline && !s_stop.load()) {
        size_t received = 0;
        const esp_err_t err = i2s_channel_read(s_rx, sample, sizeof(sample), &received,
                                               50);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "I2S RX probe stopped: %s", esp_err_to_name(err));
            break;
        }
        if (err == ESP_ERR_TIMEOUT) ++timeouts;
        // Count partial reads too: ESP_ERR_TIMEOUT does not discard bytes
        // already copied by the driver.
        if (received > 0) {
            ++chunks;
            const int64_t now = esp_timer_get_time();
            if (first == 0) {
                first = now;
                continue;
            }
            bytes += received;
            last = now;
        }
    }
    const int64_t window = last > first ? last - first : 0;
    if (window < 200000 || bytes < 2048) {
        ESP_LOGW(TAG, "I2S rate probe: chunks=%u timeouts=%u bytes=%llu window=%lld ms",
                 chunks, timeouts, (unsigned long long)bytes,
                 (long long)(window / 1000));
        return 0;
    }
    const uint32_t rate = (uint32_t)((bytes / 8) * 1000000ull / (uint64_t)window);
    const uint32_t snapped = snap_rate(rate);
    ESP_LOGI(TAG, "BH1098G master rate measured=%u Hz snapped=%u Hz "
             "(chunks=%u timeouts=%u window=%lld ms)",
             rate, snapped, chunks, timeouts, (long long)(window / 1000));
    return snapped;
}

// The reference board wires the ESP as an I2S slave and assumes the BH1098G
// drives BCLK/WS.  On this build the module leaves both lines high-Z, so use
// the module's own pin direction to pick the role: it is safe to output the
// clock only when neither line is driven.  Anything else keeps the ESP as a
// slave to avoid two masters fighting over the bus.
bool configure_sink_clock()
{
    const bool bclk_driven = probe_pin_driven((gpio_num_t)pins::i2s_bclk, "BCLK");
    const bool ws_driven = probe_pin_driven((gpio_num_t)pins::i2s_ws, "WS");
    const bool esp_master = !bclk_driven && !ws_driven;
    if (!esp_master && (!bclk_driven || !ws_driven)) {
        ESP_LOGW(TAG, "BH1098G drives only one of BCLK/WS; staying I2S slave");
    }
    if (!create_i2s(esp_master)) return false;
    if (esp_master) {
        // No measurement is possible (and none is needed) when the ESP owns
        // the clock: the reference path runs the local I2S link at 16 kHz.
        s_output_rate.store(DEFAULT_OUTPUT_RATE);
        ESP_LOGI(TAG, "BH1098G leaves BCLK/WS high-Z; ESP is I2S master at %u Hz",
                 (unsigned)DEFAULT_OUTPUT_RATE);
        return true;
    }
    uint32_t detected = measure_master_rate();
    if (detected == 0) {
        // The chip may answer AT+MODE=1 before its I2S clock is running.
        vTaskDelay(pdMS_TO_TICKS(300));
        detected = measure_master_rate();
    }
    if (detected != 0) s_output_rate.store(detected);
    ESP_LOGI(TAG, "output rate %u Hz (measured=%u Hz)",
             (unsigned)s_output_rate.load(), (unsigned)detected);
    return true;
}

void bt_init_task(void *)
{
    // sink_init() just switched the rail on: wait for the module to boot
    // before the first AT, otherwise the command is silently dropped.
    vTaskDelay(pdMS_TO_TICKS(BT_BOOT_SETTLE_MS));
    uart_send("AT+RX=2\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    if (!uart_log_reply(150)) {
        // This command selects the local I2S audio path.  If it was lost the
        // module still answers AT+MODE=1 but stays silent, so resend it once.
        uart_send("AT+RX=2\r\n");
        vTaskDelay(pdMS_TO_TICKS(700));
        (void)uart_log_reply(150);
    }
    uart_send("AT+MODE=1\r\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    (void)uart_log_reply(150);

    // I2S is created only now: the module is awake and in mode 1, so the pin
    // probe sees its final clock direction instead of a powered-down board.
    if (!configure_sink_clock()) {
        ESP_LOGE(TAG, "I2S setup failed; speaker playback unavailable");
    }
    s_bt_ready.store(true);
    s_bt_running.store(false);
    xSemaphoreGive(s_bt_done);
    vTaskDelete(nullptr);
}

bool sink_init()
{
    // PA off before anything else, then power the BT/PA rail.
    if (!pca_output_bit(PCA_PORT0_OUTPUT, PCA_PORT0_CONFIG, PCA_PA_MASK, false)) {
        return false;
    }
    if (!pca_output_bit(PCA_PORT1_OUTPUT, PCA_PORT1_CONFIG, PCA_BT_POWER_MASK, true)) {
        return false;
    }
    ESP_LOGI(TAG, "BT/PA rail on (PCA P1.4)");
    // I2S is created by bt_init_task once the module has entered mode 1 and
    // its clock direction is known.
    if (!start_uart()) return false;
    return true;
}

void sink_deinit()
{
    destroy_i2s();
    if (s_uart_installed) {
        uart_driver_delete(UART_NUM_1);
        s_uart_installed = false;
    }
    pa_set(false);
}

// ---------------------------------------------------------------- settings --

void load_settings()
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value = 0;
    if (nvs_get_u8(handle, NVS_SPK_VOLUME_KEY, &value) == ESP_OK && value <= 100) {
        s_volume.store(value);
    }
    if (nvs_get_u8(handle, NVS_MODE_KEY, &value) == ESP_OK &&
        value <= AUDIO_OUTPUT_AUTO) {
        // USB output is not wired on S31 yet; never leave playback pointed at
        // a sink that cannot be opened.
        s_output_mode.store(value == (uint8_t)AUDIO_OUTPUT_USB
                                ? (int)AUDIO_OUTPUT_SPEAKER
                                : (int)value);
    }
    nvs_close(handle);
}

void save_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, key, value) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

// ------------------------------------------------------------------ player --

struct Job {
    FILE *file = nullptr;
    esp_audio_simple_dec_type_t type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    uint32_t size = 0;
};

esp_audio_simple_dec_type_t type_from_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == nullptr) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    if (strcasecmp(dot, ".mp3") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (strcasecmp(dot, ".flac") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (strcasecmp(dot, ".wav") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (strcasecmp(dot, ".m4a") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (strcasecmp(dot, ".aac") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (strcasecmp(dot, ".ogg") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    if (strcasecmp(dot, ".opus") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

bool play_pcm(Converters &converters, esp_audio_simple_dec_handle_t decoder,
              const uint8_t *data, size_t length)
{
    esp_audio_simple_dec_info_t info = {};
    if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "decoder info not available yet");
        }
        return true;
    }
    if (info.sample_rate == 0 || info.channel == 0 || info.bits_per_sample < 8) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "decoder info incomplete (rate=%u ch=%u bits=%u)",
                     (unsigned)info.sample_rate, info.channel, info.bits_per_sample);
        }
        return true;
    }
    const uint32_t target = s_output_rate.load();
    if (!converters.configure(info.sample_rate, info.channel, info.bits_per_sample,
                              target)) {
        return false;
    }
    const uint8_t *output = nullptr;
    uint32_t frames = 0;
    if (!converters.process(data, length, &output, &frames)) return false;
    return write_pcm(output, frames);
}

void network_log_link(const char *stage)
{
    wifi_ap_record_t ap = {};
    wifi_ps_type_t ps = WIFI_PS_NONE;
    uint8_t protocol = 0;
    const esp_err_t link = esp_wifi_sta_get_ap_info(&ap);
    const esp_err_t power = esp_wifi_get_ps(&ps);
    const esp_err_t protocol_result = esp_wifi_get_protocol(WIFI_IF_STA, &protocol);
    esp_netif_ip_info_t ip = {};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip);
    ESP_LOGI(TAG, "radio link %s: STA=%s rssi=%d channel=%u protocol=0x%02x/%s ps=%d/%s ip=" IPSTR " gateway=" IPSTR,
             stage, esp_err_to_name(link), (int)ap.rssi, (unsigned)ap.primary,
             (unsigned)protocol, esp_err_to_name(protocol_result), (int)ps,
             esp_err_to_name(power), IP2STR(&ip.ip), IP2STR(&ip.gw));
    esp_netif_t *default_netif = esp_netif_get_default_netif();
    ESP_LOGI(TAG, "radio default route: interface=%s is_wifi=%d",
             default_netif ? esp_netif_get_desc(default_netif) : "none", default_netif == netif && netif != nullptr);
    for (int i = 0; netif && i < 2; ++i) {
        esp_netif_dns_info_t dns = {};
        if (esp_netif_get_dns_info(netif, i == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4) {
            ESP_LOGI(TAG, "radio DNS server[%d]=" IPSTR, i, IP2STR(&dns.ip.u_addr.ip4));
        }
    }
}

int network_http_event(http_stream_event_msg_t *event)
{
    if (!event || event->event_id != HTTP_STREAM_PRE_REQUEST ||
        s_network_endpoint_logged.exchange(true)) return ESP_GMF_ERR_OK;
    // One diagnostic lookup per station, with the same family/socket hints as
    // ESP-TLS. Do not replace URLs/Host/SNI or silently change routing policy.
    char url[512];
    auto client = static_cast<esp_http_client_handle_t>(event->http_client);
    if (esp_http_client_get_url(client, url, sizeof(url)) != ESP_OK) return ESP_GMF_ERR_OK;
    esp_gmf_uri_t *uri = nullptr;
    if (esp_gmf_uri_parse(url, &uri) != 0 || !uri || !uri->host) {
        if (uri) esp_gmf_uri_free(uri);
        return ESP_GMF_ERR_OK;
    }
    addrinfo hints = {}, *addresses = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const int64_t begin = esp_timer_get_time();
    const int result = getaddrinfo(uri->host, nullptr, &hints, &addresses);
    ESP_LOGI(TAG, "radio resolve: host=%s port=%d result=%d time=%lld ms",
             uri->host, uri->port, result, (long long)((esp_timer_get_time() - begin) / 1000));
    int index = 0;
    for (auto *address = addresses; address && index < 4; address = address->ai_next, ++index) {
        char text[INET6_ADDRSTRLEN] = {};
        const void *ip = nullptr;
        if (address->ai_family == AF_INET)
            ip = &reinterpret_cast<sockaddr_in *>(address->ai_addr)->sin_addr;
        else if (address->ai_family == AF_INET6)
            ip = &reinterpret_cast<sockaddr_in6 *>(address->ai_addr)->sin6_addr;
        if (ip && inet_ntop(address->ai_family, ip, text, sizeof(text))) {
            ESP_LOGI(TAG, "radio endpoint[%d]: family=%s address=%s sdk_default_first=%d", index,
                     address->ai_family == AF_INET ? "IPv4" : "IPv6", text, index == 0);
        }
    }
    if (addresses) freeaddrinfo(addresses);
    esp_gmf_uri_free(uri);
    return ESP_GMF_ERR_OK;
}

void network_acquire_wifi()
{
    wifi_ps_type_t previous;
    if (esp_wifi_get_ps(&previous) == ESP_OK && previous != WIFI_PS_NONE) {
        const esp_err_t result = esp_wifi_set_ps(WIFI_PS_NONE);
        if (result == ESP_OK) {
            s_network_previous_ps = previous;
            s_network_restore_ps = true;
        } else ESP_LOGW(TAG, "radio Wi-Fi power-save disable failed: %s", esp_err_to_name(result));
    }
    network_log_link("start");
}

void network_release_wifi()
{
    if (!s_network_restore_ps) return;
    const esp_err_t result = esp_wifi_set_ps(s_network_previous_ps);
    ESP_LOGI(TAG, "radio Wi-Fi power-save restore: mode=%d result=%s",
             (int)s_network_previous_ps, esp_err_to_name(result));
    s_network_restore_ps = false;
}

int network_write(uint8_t *data, int length, void *)
{
    // Consume callbacks arriving during teardown without touching I2S.
    if (!s_network_write_enabled.load()) return length;
    if (!data || length <= 0 || length % 4) return -1;
    return write_pcm(data, uint32_t(length / 4)) ? length : -1;
}

int network_prepare(esp_asp_handle_t *handle, void *)
{
    // The ASP callback passes its opaque handle directly (same API as P4).
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    esp_gmf_element_handle_t rate = nullptr, channel = nullptr, bits = nullptr;
    if (esp_audio_simple_player_get_pipeline(reinterpret_cast<esp_asp_handle_t>(handle), &pipeline) != ESP_GMF_ERR_OK ||
        !pipeline ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_rate_cvt", &rate) != ESP_GMF_ERR_OK || !rate ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_ch_cvt", &channel) != ESP_GMF_ERR_OK || !channel ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_bit_cvt", &bits) != ESP_GMF_ERR_OK || !bits) return -1;
    // BH1098G owns the hardware clock: upsample low-rate stations too, rather
    // than applying the adaptive upper limit used by the P4 codec.
    if (esp_gmf_rate_cvt_clear_max_dest_rate(rate) != ESP_GMF_ERR_OK ||
        esp_gmf_rate_cvt_set_dest_rate(rate, s_output_rate.load()) != ESP_GMF_ERR_OK ||
        esp_gmf_ch_cvt_set_dest_channel(channel, 2) != ESP_GMF_ERR_OK ||
        esp_gmf_bit_cvt_set_dest_bits(bits, 16) != ESP_GMF_ERR_OK) return -1;
    ESP_LOGI(TAG, "radio PCM target: %u Hz stereo 16 bit -> BH1098G",
             (unsigned)s_output_rate.load());
    return 0;
}

int network_event(esp_asp_event_pkt_t *event, void *)
{
    if (!event || !event->payload || !s_network_active.load()) return 0;
    if (event->type == ESP_ASP_EVENT_TYPE_STATE) {
        const auto state = *static_cast<esp_asp_state_t *>(event->payload);
        if (state == ESP_ASP_STATE_RUNNING) s_state.store(AUDIO_PLAYBACK_PLAYING);
        else if (state == ESP_ASP_STATE_PAUSED) s_state.store(AUDIO_PLAYBACK_PAUSED);
        else if (state == ESP_ASP_STATE_STOPPED) s_state.store(AUDIO_PLAYBACK_STOPPED);
        else if (state == ESP_ASP_STATE_FINISHED || state == ESP_ASP_STATE_ERROR) {
            s_network_write_enabled.store(false);
            s_state.store(state == ESP_ASP_STATE_ERROR ? AUDIO_PLAYBACK_ERROR : AUDIO_PLAYBACK_FINISHED);
            pa_set(false);
            if (state == ESP_ASP_STATE_ERROR) network_log_link("error");
        }
        ESP_LOGI(TAG, "radio state: %s", esp_audio_simple_player_state_to_str(state));
    } else if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO) {
        auto *info = static_cast<esp_asp_music_info_t *>(event->payload);
        ESP_LOGI(TAG, "radio decoded PCM: %d Hz %u ch %u bit", info->sample_rate,
                 (unsigned)info->channels, (unsigned)info->bits);
    }
    return 0;
}

int network_hls_format(esp_hls_file_seg_info_t *info, void *context)
{
    if (!info || !context || !info->format || info->format == UINT32_MAX) return -1;
    if (s_hls_format == info->format) return 0;
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    esp_gmf_element_handle_t decoder = nullptr;
    if (esp_audio_simple_player_get_pipeline(context, &pipeline) != ESP_GMF_ERR_OK || !pipeline ||
        esp_gmf_pipeline_get_el_by_name(pipeline, "aud_dec", &decoder) != ESP_GMF_ERR_OK || !decoder) return -1;
    esp_gmf_info_sound_t sound = {};
    sound.format_id = info->format;
    sound.sample_rates = s_output_rate.load();
    sound.channels = 2;
    sound.bits = 16;
    if (esp_gmf_audio_dec_reconfig_by_sound_info(decoder, &sound) != ESP_GMF_ERR_OK) return -1;
    s_hls_format = info->format;
    ESP_LOGI(TAG, "radio HLS audio format: %c%c%c%c", char(info->format),
             char(info->format >> 8), char(info->format >> 16), char(info->format >> 24));
    return 0;
}

bool stop_network_locked()
{
    s_network_write_enabled.store(false);
    s_network_active.store(false);
    if (!s_network_player) { network_release_wifi(); return true; }
    esp_asp_state_t state = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_network_player, &state);
    if (state == ESP_ASP_STATE_RUNNING || state == ESP_ASP_STATE_PAUSED) {
        const auto ret = esp_audio_simple_player_stop(s_network_player);
        if (ret != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "radio stop incomplete: %d; retaining player for retry", (int)ret);
            return false;
        }
    }
    if (esp_audio_simple_player_destroy(s_network_player) != ESP_GMF_ERR_OK) return false;
    s_network_player = nullptr;
    // ASP unregisters the shared decoder registry when its last player dies.
    s_decoders_registered = false;
    s_hls_format = 0;
    s_network_endpoint_logged.store(false);
    network_release_wifi();
    return true;
}

bool create_network_locked()
{
    static bool adapter_ready = false;
    if (!adapter_ready) { media_lib_add_default_adapter(); adapter_ready = true; }
    // Local and network playback are serialized; transfer decoder-registry
    // ownership to ASP so a later destroy cannot invalidate a local decoder.
    if (s_decoders_registered) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        s_decoders_registered = false;
    }
    esp_asp_cfg_t config = {};
    config.out.cb = network_write;
    config.task_prio = WORKER_PRIORITY;
    config.task_core = 0;
    config.task_stack = 8192;
    config.task_stack_in_ext = true;
    config.prev = network_prepare;
    if (esp_audio_simple_player_new(&config, &s_network_player) != ESP_GMF_ERR_OK) return false;
    esp_gmf_pool_handle_t pool = nullptr;
    if (esp_audio_simple_player_get_pool(s_network_player, &pool) != ESP_GMF_ERR_OK || !pool) return false;
    http_io_cfg_t http = HTTP_STREAM_CFG_DEFAULT();
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.event_handle = network_http_event;
    esp_gmf_io_handle_t http_io = nullptr;
    if (esp_gmf_io_http_init(&http, &http_io) != ESP_GMF_ERR_OK) return false;
    if (esp_audio_simple_player_register_io(s_network_player, http_io) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(http_io);
        return false;
    }
    esp_hls_io_cfg_t hls = {};
    hls.name = "io_hls";
    hls.pool = pool;
    hls.file_seg_cb = network_hls_format;
    hls.ctx = s_network_player;
    hls.io_cfg.thread.stack = 20 * 1024;
    hls.io_cfg.thread.prio = 9;
    hls.io_cfg.thread.core = 0;
    hls.io_cfg.thread.stack_in_ext = true;
    hls.io_cfg.buffer_cfg.buffer_size = 128 * 1024;
    esp_gmf_io_handle_t hls_io = nullptr;
    if (esp_gmf_io_hls_init(&hls, &hls_io) != ESP_GMF_ERR_OK) return false;
    if (esp_audio_simple_player_register_io(s_network_player, hls_io) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(hls_io);
        return false;
    }
    return esp_audio_simple_player_set_event(s_network_player, network_event, nullptr) == ESP_GMF_ERR_OK;
}

bool join_worker_locked()
{
    // Cancel metadata work before changing or stopping the current song.
    ++s_duration_generation;
    s_stop.store(true);
    if (!stop_network_locked()) return false;
    if (s_worker == nullptr) {
        (void)xSemaphoreTake(s_exited, 0);
        s_stop.store(false);
        s_pause.store(false);
        return true;
    }
    s_stop.store(true);
    if (xSemaphoreTake(s_exited, pdMS_TO_TICKS(3000)) != pdTRUE) {
        // A stuck write with no master clock would otherwise freeze the UI.
        ESP_LOGW(TAG, "worker did not stop in time; deleting task");
        vTaskDelete(s_worker);
    }
    s_worker = nullptr;
    s_stop.store(false);
    s_pause.store(false);
    return true;
}

struct DurationJob {
    char *path;
    uint32_t generation;
};
struct DurationScan {
    uint32_t generation;
    int64_t last_yield;
};

bool duration_progress(void *context)
{
    auto *scan = static_cast<DurationScan *>(context);
    if (scan->generation != s_duration_generation.load() || !s_initialized.load()) return false;
    const int64_t now = esp_timer_get_time();
    if (now - scan->last_yield >= 20000) {
        vTaskDelay(1);
        scan->last_yield = esp_timer_get_time();
    }
    return scan->generation == s_duration_generation.load();
}

void duration_task(void *)
{
    DurationJob job = {};
    for (;;) {
        if (xQueueReceive(s_duration_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        DurationScan scan{job.generation, esp_timer_get_time()};
        // Allow decoding/output to start before metadata scans compete for SD.
        while (duration_progress(&scan) && s_played_frames.load() == 0 &&
               s_state.load() == AUDIO_PLAYBACK_PLAYING) vTaskDelay(pdMS_TO_TICKS(20));
        uint32_t duration = 0;
        AudioFileDurationDetails details;
        const int64_t begin = esp_timer_get_time();
        if (duration_progress(&scan)) {
            FILE *file = fopen(job.path, "rb");
            if (file) {
                audio_file_duration_read(file, job.path, &duration, duration_progress, &scan, &details);
                fclose(file);
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool current = job.generation == s_duration_generation.load() && s_initialized.load();
        if (current && duration) s_duration_ms.store(duration);
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "duration metadata: applied=%d duration=%u ms time=%lld ms path=%s",
                 current && duration != 0, (unsigned)duration,
                 (long long)((esp_timer_get_time() - begin) / 1000), job.path);
        if (current && !duration) {
            ESP_LOGW(TAG, "duration unavailable: stage=%s reason=%s first=%ld frames=%u reads=%u",
                     details.stage, details.failure, details.first_frame,
                     (unsigned)details.frames, (unsigned)details.reads);
        }
        free(job.path);
    }
}

// Called with s_lock held. Queue owns the path until consumed/replaced.
void queue_duration(const char *path)
{
    if (!s_duration_queue) {
        s_duration_queue = xQueueCreate(1, sizeof(DurationJob));
        if (!s_duration_queue) return;
        if (xTaskCreate(duration_task, "s31_duration", 4096, nullptr, 2, nullptr) != pdPASS) {
            vQueueDelete(s_duration_queue);
            s_duration_queue = nullptr;
            ESP_LOGW(TAG, "duration worker creation failed");
            return;
        }
    }
    DurationJob job{strdup(path), s_duration_generation.load()};
    if (!job.path) return;
    DurationJob pending = {};
    if (xQueueReceive(s_duration_queue, &pending, 0) == pdTRUE) free(pending.path);
    if (xQueueSend(s_duration_queue, &job, 0) != pdTRUE) free(job.path);
}

void worker_task(void *arg)
{
    auto *job = static_cast<Job *>(arg);
    esp_audio_simple_dec_handle_t decoder = nullptr;
    Converters converters;
    uint8_t *input = static_cast<uint8_t *>(malloc(INPUT_CHUNK_BYTES));
    uint8_t *output = static_cast<uint8_t *>(malloc(OUTPUT_CHUNK_INITIAL));
    size_t output_capacity = OUTPUT_CHUNK_INITIAL;
    bool ok = input != nullptr && output != nullptr;
    bool failed = false;

    if (!s_decoders_registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        s_decoders_registered = true;
    }
    if (ok) {
        esp_audio_simple_dec_cfg_t cfg = {};
        cfg.dec_type = job->type;
        cfg.use_frame_dec = false;
        if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "simple decoder open failed for type %d", (int)job->type);
            ok = false;
            failed = true;
        }
    }

    if (ok) {
        // The first playback is also what powers the BT/PA rail and applies
        // mode 1 (see bt_init_task).  Do not feed I2S until that finished:
        // writing without a master clock only spins on timeouts.
        const int64_t wait_started = esp_timer_get_time();
        while (!s_bt_ready.load() && !s_stop.load() &&
               esp_timer_get_time() - wait_started < 6000000) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ESP_LOGI(TAG, "PCM path %s after %lld ms (rate=%u Hz, volume=%d)",
                 s_bt_ready.load() ? "ready" : "NOT ready",
                 (long long)((esp_timer_get_time() - wait_started) / 1000),
                 (unsigned)s_output_rate.load(), s_volume.load());
        pa_set(true);
        while (!s_stop.load()) {
            if (s_pause.load()) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            const size_t length = fread(input, 1, INPUT_CHUNK_BYTES, job->file);
            if (length == 0) {
                if (ferror(job->file)) {
                    ESP_LOGE(TAG, "file read failed");
                    failed = true;
                }
                break;
            }
            const bool eos = length < INPUT_CHUNK_BYTES || feof(job->file);
            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = input;
            raw.len = length;
            raw.eos = eos;

            while (raw.len > 0 && !s_stop.load()) {
                esp_audio_simple_dec_out_t frame = {};
                frame.buffer = output;
                frame.len = output_capacity;
                const esp_audio_err_t result =
                    esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    if (frame.needed_size <= output_capacity ||
                        frame.needed_size > OUTPUT_CHUNK_MAX) {
                        ESP_LOGE(TAG, "decoder needs an unusable output buffer (%u)",
                                 (unsigned)frame.needed_size);
                        failed = true;
                        break;
                    }
                    auto *grown = static_cast<uint8_t *>(realloc(output, frame.needed_size));
                    if (grown == nullptr) {
                        ESP_LOGE(TAG, "decoder output buffer allocation failed");
                        failed = true;
                        break;
                    }
                    output = grown;
                    output_capacity = frame.needed_size;
                    continue;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    ESP_LOGE(TAG, "decode failed: %d", (int)result);
                    failed = true;
                    break;
                }
                if (frame.decoded_size > 0 &&
                    !play_pcm(converters, decoder, frame.buffer, frame.decoded_size)) {
                    if (!s_stop.load()) {
                        ESP_LOGE(TAG, "PCM output failed");
                        failed = true;
                    }
                    break;
                }
                const uint32_t consumed = raw.consumed;
                raw.buffer += consumed;
                raw.len -= consumed;
                // No progress means the decoder wants more input.
                if (consumed == 0 && frame.decoded_size == 0) break;
            }
            s_file_pos.store((uint32_t)ftell(job->file));
            if (failed || s_stop.load()) break;
            if (eos) break;
        }
        // Let the DMA ring drain before cutting the amplifier, otherwise the
        // last fraction of a second is lost.
        if (!s_stop.load()) vTaskDelay(pdMS_TO_TICKS(120));
        pa_set(false);
    }

    if (decoder) esp_audio_simple_dec_close(decoder);
    converters.close();
    free(input);
    free(output);
    if (job->file) fclose(job->file);
    delete job;

    const audio_playback_state_t final_state =
        failed ? AUDIO_PLAYBACK_ERROR
               : (s_stop.load() ? AUDIO_PLAYBACK_STOPPED : AUDIO_PLAYBACK_FINISHED);
    s_state.store(final_state);
    xSemaphoreGive(s_exited);
    vTaskDelete(nullptr);
}

}  // namespace

// ------------------------------------------------------------------- API ----

extern "C" {

esp_err_t audio_radio_init(void)
{
    if (s_initialized.load()) return ESP_OK;
    ensure_sync();
    load_settings();

    if (!sink_init()) {
        sink_deinit();
        return ESP_FAIL;
    }
    s_initialized.store(true);
    s_state.store(AUDIO_PLAYBACK_IDLE);
    s_played_frames.store(0);
    s_file_pos.store(0);
    s_file_size.store(0);

    if (!s_bt_running.exchange(true)) {
        (void)xSemaphoreTake(s_bt_done, 0);
        s_bt_ready.store(false);
        if (xTaskCreate(bt_init_task, "s31_bt_init", 3072, nullptr, 3, nullptr) != pdPASS) {
            ESP_LOGW(TAG, "BT init task creation failed");
            s_bt_ready.store(true);  // do not make playback wait for nothing
            s_bt_running.store(false);
            xSemaphoreGive(s_bt_done);
        }
    }
    ESP_LOGI(TAG, "ready: BH1098G backend, PA on PCA9535 P0.4, volume=%d",
             s_volume.load());
    return ESP_OK;
}

esp_err_t audio_radio_play(const char *url)
{
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) return ESP_ERR_INVALID_ARG;
    const esp_err_t init = audio_radio_init();
    if (init != ESP_OK) return init;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!join_worker_locked()) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    s_played_frames.store(0);
    s_duration_ms.store(0);
    s_file_pos.store(0);
    s_file_size.store(0);
    const int64_t deadline = esp_timer_get_time() + 6000000;
    while (!s_bt_ready.load() && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_bt_ready.load() && s_tx) network_acquire_wifi();
    if (!s_bt_ready.load() || !s_tx || !create_network_locked()) {
        stop_network_locked();
        s_state.store(AUDIO_PLAYBACK_ERROR);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    s_state.store(AUDIO_PLAYBACK_PLAYING);
    s_network_active.store(true);
    s_network_write_enabled.store(true);
    pa_set(true);
    const auto result = esp_audio_simple_player_run(s_network_player, url, nullptr);
    if (result != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "radio start failed: %d", (int)result);
        stop_network_locked();
        pa_set(false);
        s_state.store(AUDIO_PLAYBACK_ERROR);
    }
    xSemaphoreGive(s_lock);
    return result == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_radio_play_file(const char *path)
{
    if (path == nullptr) return ESP_ERR_INVALID_ARG;
    const esp_audio_simple_dec_type_t type = type_from_path(path);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) return ESP_ERR_NOT_SUPPORTED;

    esp_err_t err = audio_radio_init();
    if (err != ESP_OK) return err;
    if (s_output_mode.load() == AUDIO_OUTPUT_USB) return ESP_ERR_NOT_SUPPORTED;

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    if (size <= 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    auto *job = new (std::nothrow) Job();
    if (job == nullptr) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    job->file = file;
    job->type = type;
    job->size = (uint32_t)size;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!join_worker_locked()) {
        xSemaphoreGive(s_lock);
        fclose(file);
        delete job;
        return ESP_FAIL;
    }
    s_played_frames.store(0);
    s_duration_ms.store(0);
    s_file_pos.store(0);
    s_file_size.store((uint32_t)size);
    s_state.store(AUDIO_PLAYBACK_PLAYING);
    TaskHandle_t handle = nullptr;
    const BaseType_t created = xTaskCreate(worker_task, "s31_audio", WORKER_STACK_BYTES,
                                           job, WORKER_PRIORITY, &handle);
    if (created != pdPASS) {
        s_state.store(AUDIO_PLAYBACK_ERROR);
        xSemaphoreGive(s_lock);
        fclose(file);
        delete job;
        return ESP_ERR_NO_MEM;
    }
    s_worker = handle;
    queue_duration(path);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "playing %s (%u bytes, type %d)", path, (unsigned)size, (int)type);
    return ESP_OK;
}

esp_err_t audio_radio_play_default(void)
{
    return audio_radio_play("http://ngcdn001.cnr.cn/live/zgzs/index.m3u8");
}

void audio_radio_force_speaker(bool)
{
    // The S31 backend always uses the speaker until the USB headset path lands.
}

esp_err_t audio_radio_stop(void)
{
    if (!s_initialized.load()) return ESP_OK;
    ensure_sync();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool stopped = join_worker_locked();
    s_state.store(stopped ? AUDIO_PLAYBACK_STOPPED : AUDIO_PLAYBACK_ERROR);
    pa_set(false);
    xSemaphoreGive(s_lock);
    return stopped ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_radio_pause(void)
{
    if (!s_initialized.load()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int state = s_state.load();
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (state == AUDIO_PLAYBACK_PAUSED) result = ESP_OK;
    else if (state == AUDIO_PLAYBACK_PLAYING) {
        if (s_network_player) result = esp_audio_simple_player_pause(s_network_player) == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
        else if (s_worker) { s_pause.store(true); result = ESP_OK; }
        if (result == ESP_OK) s_state.store(AUDIO_PLAYBACK_PAUSED);
    }
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t audio_radio_resume(void)
{
    if (!s_initialized.load()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_state.load() == AUDIO_PLAYBACK_PAUSED) {
        if (s_network_player) result = esp_audio_simple_player_resume(s_network_player) == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
        else if (s_worker) { s_pause.store(false); result = ESP_OK; }
        if (result == ESP_OK) s_state.store(AUDIO_PLAYBACK_PLAYING);
    }
    xSemaphoreGive(s_lock);
    return result;
}

audio_playback_state_t audio_radio_get_state(void)
{
    return (audio_playback_state_t)s_state.load();
}

bool audio_radio_is_playback_ready(void)
{
    return s_initialized.load() && (s_network_active.load()
        ? s_network_write_enabled.load() && s_played_frames.load() > 0 : s_worker != nullptr);
}

esp_err_t audio_radio_get_file_progress(uint32_t *progress_permille)
{
    if (progress_permille == nullptr) return ESP_ERR_INVALID_ARG;
    const uint32_t size = s_file_size.load();
    if (size == 0) {
        *progress_permille = 0;
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint64_t position = s_file_pos.load();
    if (position > size) position = size;
    *progress_permille = (uint32_t)(position * 1000 / size);
    return ESP_OK;
}

esp_err_t audio_radio_get_file_timing(uint32_t *elapsed_ms, uint32_t *duration_ms)
{
    if (elapsed_ms == nullptr || duration_ms == nullptr) return ESP_ERR_INVALID_ARG;
    const uint32_t rate = s_output_rate.load();
    const uint64_t frames = s_played_frames.load();
    *elapsed_ms = rate ? (uint32_t)(frames * 1000 / rate) : 0;
    *duration_ms = s_duration_ms.load();
    return s_initialized.load() ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_radio_set_volume(int volume)
{
    if (volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    s_volume.store(volume);
    save_u8(NVS_SPK_VOLUME_KEY, (uint8_t)volume);
    ESP_LOGI(TAG, "speaker volume %d", volume);
    return ESP_OK;
}

int audio_radio_get_volume(void)
{
    return s_volume.load();
}

esp_err_t audio_radio_set_output_mode(audio_output_mode_t mode)
{
    if (mode < AUDIO_OUTPUT_SPEAKER || mode > AUDIO_OUTPUT_AUTO) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode == AUDIO_OUTPUT_USB) {
        // Reserved for the later USB-headset implementation.
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_output_mode.store(mode);
    save_u8(NVS_MODE_KEY, (uint8_t)mode);
    return ESP_OK;
}

audio_output_mode_t audio_radio_get_output_mode(void)
{
    return (audio_output_mode_t)s_output_mode.load();
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
    return "SPK";
}

bool audio_radio_usb_connected(void)
{
    return false;
}

void audio_radio_service_output(void)
{
    // No headset sink to reconcile yet.
}

esp_err_t audio_radio_record_wav(const char *, uint32_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_radio_deinit(void)
{
    if (!s_initialized.exchange(false)) return;
    ensure_sync();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!join_worker_locked()) {
        // Keep the sink alive if the network tasks could not be joined safely.
        s_initialized.store(true);
        s_state.store(AUDIO_PLAYBACK_ERROR);
        pa_set(false);
        xSemaphoreGive(s_lock);
        return;
    }
    s_state.store(AUDIO_PLAYBACK_IDLE);
    pa_set(false);
    xSemaphoreGive(s_lock);

    if (s_bt_running.load()) {
        if (xSemaphoreTake(s_bt_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGW(TAG, "BT init task still running during deinit");
        }
    }
    s_bt_ready.store(false);
    sink_deinit();
    ESP_LOGI(TAG, "deinitialized");
}

void *audio_radio_get_player(void)
{
    return s_network_player;
}

}  // extern "C"
