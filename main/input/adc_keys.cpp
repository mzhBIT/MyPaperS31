#include "adc_keys.h"

#include <M5Unified.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>
#include <esp_log.h>

static const char *TAG = "ADC_KEYS";

static constexpr adc_unit_t ADC_KEY_UNIT = ADC_UNIT_1;
static constexpr adc_channel_t ADC_KEY_CHANNEL = ADC_CHANNEL_0; // GPIO1 on ESP32-S3
static constexpr adc_atten_t ADC_KEY_ATTEN = ADC_ATTEN_DB_12;

static adc_oneshot_unit_handle_t s_adc_handle = nullptr;
static m5::Button_Class s_buttons[5];
static int s_press_raw[5] = {};
static AppKey s_pending[8];
static int s_head = 0;
static int s_tail = 0;

static void push_key(AppKey key)
{
    int next = (s_tail + 1) % 8;
    if (next == s_head) return;
    s_pending[s_tail] = key;
    s_tail = next;
}

static int read_avg(int samples)
{
    int sum = 0;
    int valid = 0;
    for (int i = 0; i < samples; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, ADC_KEY_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
            ++valid;
        }
    }
    return valid ? sum / valid : 4095;
}

static AppKey decode_key(int raw)
{
    // Measured values:
    // BACK=0, UP=524, DOWN=1241, OK=1941, EXTRA=2869, idle is above the last threshold.
    if (raw < 262) return APP_KEY_BACK;
    if (raw < 882) return APP_KEY_UP;
    if (raw < 1591) return APP_KEY_DOWN;
    if (raw < 2405) return APP_KEY_OK;
    if (raw < 3484) return APP_KEY_EXTRA;
    return APP_KEY_NONE;
}

void adc_keys_init()
{
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_KEY_UNIT;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    chan_config.atten = ADC_KEY_ATTEN;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_KEY_CHANNEL, &chan_config));

    for (auto &button : s_buttons) {
        button.setDebounceThresh(30);
        button.setHoldThresh(500);
    }

    ESP_LOGI(TAG, "initialized on GPIO1 / ADC1_CH0");
}

void adc_keys_update()
{
    if (!s_adc_handle) return;

    int raw = read_avg(8);
    AppKey current = decode_key(raw);
    uint32_t now = lgfx::millis();

    for (int i = 0; i < 5; ++i) {
        AppKey key = static_cast<AppKey>(i + 1);
        s_buttons[i].setRawState(now, current == key);
        if (s_buttons[i].wasPressed()) {
            s_press_raw[i] = raw;
        }
        if (s_buttons[i].wasClicked()) {
            push_key(key);
            ESP_LOGI(TAG, "key=%d press_raw=%d release_raw=%d", key, s_press_raw[i], raw);
        }
    }
}

bool adc_keys_pop(AppKey *key)
{
    if (s_head == s_tail) return false;
    *key = s_pending[s_head];
    s_head = (s_head + 1) % 8;
    return true;
}
