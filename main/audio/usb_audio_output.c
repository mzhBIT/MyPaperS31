#include "audio/usb_audio_output.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <wchar.h>
#include "sdkconfig.h"

#if CONFIG_MYPAPER_USB_AUDIO

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
// Keep this include in a C translation unit. usb_host_uac 1.2.0 has a C-only
// descriptor callback declaration whose parameter is named `class`.
#include "usb/uac_host.h"
#include "usb/uac2_host.h"

#define USB_AUDIO_UAC1_PCM_BUFFER_SIZE (32 * 1024)
#define USB_AUDIO_UAC1_PCM_THRESHOLD   (8 * 1024)
// Keep the UAC2 deadline-facing reservoir in internal RAM. EPD waveform DMA
// reads PSRAM heavily while refreshing; consuming USB PCM from that same bus
// can miss a 125 us HS microframe. 64 KiB is about 371 ms at 44.1k/16/stereo,
// enough to absorb normal HLS segment hand-off jitter without using 128 KiB of
// scarce internal memory.
#define USB_AUDIO_UAC2_PCM_BUFFER_SIZE (64 * 1024)
#define USB_AUDIO_UAC2_PCM_THRESHOLD   (32 * 1024)
#define USB_AUDIO_EVENT_QUEUE_LEN 20
#define USB_AUDIO_HOST_STACK      (5 * 1024)
#define USB_AUDIO_UAC_STACK       (5 * 1024)
#define USB_AUDIO_UAC2_STACK      (7 * 1024)
#define USB_AUDIO_HOST_TASK_PRIORITY 13
#define USB_AUDIO_UAC1_TASK_PRIORITY 14
#define USB_AUDIO_UAC2_TASK_PRIORITY 20

#define EVENT_HOST_READY      BIT0
#define EVENT_HOST_FAILED     BIT1
#define EVENT_UAC_TASK_DONE   BIT2
#define EVENT_HOST_TASK_DONE  BIT3
#define EVENT_DEVICE_READY    BIT4

typedef enum {
    USB_AUDIO_EVENT_DRIVER = 0,
    USB_AUDIO_EVENT_DEVICE,
    USB_AUDIO_EVENT_UAC2_DRIVER,
    USB_AUDIO_EVENT_UAC2_DEVICE,
} usb_audio_event_type_t;

typedef struct {
    usb_audio_event_type_t type;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
        } driver;
        struct {
            uac_host_device_handle_t handle;
            uac_host_device_event_t event;
        } device;
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac2_host_driver_event_t event;
        } uac2_driver;
        struct {
            uac2_host_device_handle_t handle;
            uac2_host_device_event_t event;
        } uac2_device;
    };
} usb_audio_event_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
} usb_audio_format_t;

typedef struct {
    uint32_t min;
    uint32_t max;
    uint32_t res;
} usb_audio_rate_range_t;

typedef enum {
    USB_AUDIO_BACKEND_NONE = 0,
    USB_AUDIO_BACKEND_UAC1,
    USB_AUDIO_BACKEND_UAC2,
} usb_audio_backend_t;

static const char *TAG = "USB_AUDIO";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_events;
static EventGroupHandle_t s_event_group;
static TaskHandle_t s_host_task;
static TaskHandle_t s_uac_task;
static volatile bool s_run;
static volatile bool s_uac_uninstalling;
static volatile bool s_host_shutdown;
// Cleared while deinit() detaches the application state, so a driver event that
// was already queued cannot open a device back into the UAC driver's lists.
static volatile bool s_accept_devices;
static bool s_initialized;
static bool s_uac_installed;
static bool s_uac2_installed;
// Tracks the USB Host library itself, not the task that owns it. The task can
// exit with the library still installed: usb_host_uninstall() refuses to run
// unless process_pending_flags, lib_event_flags and num_clients are all zero,
// and a non-zero return there must not be mistaken for a successful teardown.
static volatile bool s_host_lib_installed;
// Set when deinit() cannot tear the USB Host stack down. The UAC client stays
// registered in that case, so the host library is unusable for the rest of this
// boot; init() refuses to start rather than corrupt the leftover state.
static bool s_teardown_failed;
static uac_host_device_handle_t s_device;
static uac2_host_device_handle_t s_uac2_device;
static usb_audio_backend_t s_backend;
static usb_audio_rate_range_t s_uac2_rates[UAC2_MAX_SAMPLE_RATE_RANGES];
static size_t s_uac2_rate_count;
static bool s_software_volume = true;
static uac2_volume_range_t s_uac2_volume[2];
static bool s_uac2_volume_valid[2];
static usb_audio_output_status_t s_status;
// UI-side hot-plug checks must never wait behind a blocking PCM ring-buffer
// write. Keep the small piece of status they need in an atomic snapshot.
static atomic_bool s_connected_snapshot;
static usb_audio_format_t s_formats[24];
static size_t s_format_count;

static void set_connected_locked(bool connected)
{
    s_status.connected = connected;
    atomic_store_explicit(&s_connected_snapshot, connected, memory_order_release);
}

static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap %s: internal_free=%u internal_largest=%u psram_free=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void copy_product(char *dst, size_t dst_size, const wchar_t *src)
{
    if (dst == NULL || dst_size == 0) return;
    size_t out = 0;
    if (src != NULL) {
        while (*src != L'\0' && out + 1 < dst_size) {
            wchar_t ch = *src++;
            dst[out++] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '?';
        }
    }
    dst[out] = '\0';
}

static bool add_format(uint32_t rate, uint8_t channels, uint8_t bits)
{
    if (rate == 0 || channels == 0 || bits == 0) return false;
    for (size_t i = 0; i < s_format_count; ++i) {
        if (s_formats[i].sample_rate == rate && s_formats[i].channels == channels &&
            s_formats[i].bits == bits) {
            return true;
        }
    }
    if (s_format_count >= sizeof(s_formats) / sizeof(s_formats[0])) return false;
    s_formats[s_format_count++] = (usb_audio_format_t) {
        .sample_rate = rate,
        .channels = channels,
        .bits = bits,
    };
    return true;
}

static bool supports_locked(uint32_t sample_rate, uint8_t channels, uint8_t bits)
{
    for (size_t i = 0; i < s_format_count; ++i) {
        if (s_formats[i].sample_rate == sample_rate &&
            s_formats[i].channels == channels && s_formats[i].bits == bits) {
            return true;
        }
    }
    return false;
}

static void device_callback(uac_host_device_handle_t handle,
                            const uac_host_device_event_t event, void *arg)
{
    (void)arg;
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        // The UAC component invokes this callback from inside
        // _uac_host_device_disconnected(), which loops over its interface list
        // and expects the interface to be removed synchronously. Deferring the
        // close via the event queue leaves the interface in that list, so the
        // loop spins forever on the same interface and starves IDLE0 (task_wdt).
        if (s_lock == NULL) return;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_device == handle) {
            ESP_LOGW(TAG, "UAC headphone disconnected");
            esp_err_t ret = uac_host_device_close(handle);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "UAC close on disconnect failed: %s", esp_err_to_name(ret));
            }
            s_device = NULL;
            s_format_count = 0;
            if (s_backend == USB_AUDIO_BACKEND_UAC1) {
                s_backend = s_uac2_device != NULL ? USB_AUDIO_BACKEND_UAC2
                                                  : USB_AUDIO_BACKEND_NONE;
                set_connected_locked(s_uac2_device != NULL);
                s_status.streaming = false;
                s_status.sample_rate = 0;
                s_status.uac_version = s_uac2_device != NULL ? 2 : 0;
                if (!s_status.connected) xEventGroupClearBits(s_event_group, EVENT_DEVICE_READY);
            }
        }
        xSemaphoreGive(s_lock);
        return;
    }

    if (s_events == NULL) return;
    usb_audio_event_t message = {
        .type = USB_AUDIO_EVENT_DEVICE,
        .device = {.handle = handle, .event = event},
    };
    (void)xQueueSend(s_events, &message, 0);
}

static void driver_callback(uint8_t addr, uint8_t iface_num,
                            const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (s_events == NULL || event != UAC_HOST_DRIVER_EVENT_TX_CONNECTED) return;
    usb_audio_event_t message = {
        .type = USB_AUDIO_EVENT_DRIVER,
        .driver = {.addr = addr, .iface_num = iface_num, .event = event},
    };
    (void)xQueueSend(s_events, &message, 0);
}

static void uac2_device_callback(uac2_host_device_handle_t handle,
                                 const uac2_host_device_event_t event, void *arg)
{
    (void)arg;
    if (s_events == NULL) return;
    usb_audio_event_t message = {
        .type = USB_AUDIO_EVENT_UAC2_DEVICE,
        .uac2_device = {.handle = handle, .event = event},
    };
    (void)xQueueSend(s_events, &message, 0);
}

static void uac2_driver_callback(uint8_t addr, uint8_t iface_num,
                                 const uac2_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (s_events == NULL || event != UAC2_HOST_DRIVER_EVENT_TX_CONNECTED) return;
    usb_audio_event_t message = {
        .type = USB_AUDIO_EVENT_UAC2_DRIVER,
        .uac2_driver = {.addr = addr, .iface_num = iface_num, .event = event},
    };
    (void)xQueueSend(s_events, &message, 0);
}

static bool rate_in_uac2_range(uint32_t rate, const usb_audio_rate_range_t *range)
{
    if (rate < range->min || rate > range->max) return false;
    return range->res == 0 || ((rate - range->min) % range->res) == 0;
}

static bool uac2_has_alt_locked(uint8_t channels, uint8_t bits)
{
    if (s_uac2_device == NULL) return false;
    for (uint8_t alt = 1; alt <= 16; ++alt) {
        uac2_host_dev_alt_param_t param = {0};
        if (uac2_host_get_device_alt_param(s_uac2_device, alt, &param) != ESP_OK) continue;
        if (param.channels == channels && param.bit_resolution == bits) return true;
    }
    return false;
}

static bool uac2_supports_locked(uint32_t rate, uint8_t channels, uint8_t bits)
{
    if (s_backend != USB_AUDIO_BACKEND_UAC2 || !uac2_has_alt_locked(channels, bits)) {
        return false;
    }
    for (size_t i = 0; i < s_uac2_rate_count; ++i) {
        if (rate_in_uac2_range(rate, &s_uac2_rates[i])) return true;
    }
    return false;
}

static void probe_uac2_locked(void)
{
    uac2_device_info_t info = {0};
    if (uac2_host_device_get_info(s_uac2_device, &info) != ESP_OK) return;

    s_status.vid = info.vid;
    s_status.pid = info.pid;
    strlcpy(s_status.product, info.product[0] ? info.product : "USB Audio 2.0",
            sizeof(s_status.product));
    s_status.uac_version = 2;
    s_uac2_rate_count = 0;
    uac2_sample_rate_range_t ranges[UAC2_MAX_SAMPLE_RATE_RANGES] = {0};
    uint8_t range_count = UAC2_MAX_SAMPLE_RATE_RANGES;
    if (uac2_host_device_get_sample_rate_range(s_uac2_device, ranges,
                                                &range_count) == ESP_OK) {
        for (uint8_t i = 0; i < range_count && i < UAC2_MAX_SAMPLE_RATE_RANGES; ++i) {
            s_uac2_rates[s_uac2_rate_count++] = (usb_audio_rate_range_t) {
                .min = ranges[i].min, .max = ranges[i].max, .res = ranges[i].res,
            };
            ESP_LOGI(TAG, "UAC2 clock range=%" PRIu32 "..%" PRIu32 " step=%" PRIu32,
                     ranges[i].min, ranges[i].max, ranges[i].res);
        }
    }

    for (uint8_t alt = 1; alt <= 16; ++alt) {
        uac2_host_dev_alt_param_t param = {0};
        if (uac2_host_get_device_alt_param(s_uac2_device, alt, &param) != ESP_OK) continue;
        ESP_LOGI(TAG, "UAC2 TX alt=%u PCM ch=%u valid=%u subslot=%u ep=0x%02x "
                 "mps=%u interval=%u", param.alt_setting, param.channels,
                 param.bit_resolution, param.sub_slot_size, param.ep_addr,
                 param.ep_max_packet_size, param.ep_interval);
    }

    s_status.hardware_volume = false;
    memset(s_uac2_volume_valid, 0, sizeof(s_uac2_volume_valid));
    for (uint8_t channel = 1; channel <= 2; ++channel) {
        uac2_volume_range_t volume_ranges[UAC2_MAX_VOLUME_RANGES] = {0};
        uint8_t volume_count = UAC2_MAX_VOLUME_RANGES;
        if (uac2_host_device_get_volume_range(s_uac2_device, channel, volume_ranges,
                                               &volume_count) == ESP_OK && volume_count > 0) {
            s_uac2_volume[channel - 1] = volume_ranges[0];
            s_uac2_volume_valid[channel - 1] = true;
            ESP_LOGI(TAG, "UAC2 Feature Unit volume ch=%u min=%d max=%d res=%d (1/256 dB)",
                     channel, volume_ranges[0].min, volume_ranges[0].max,
                     volume_ranges[0].res);
        }
    }
    if (s_uac2_volume_valid[0] && s_uac2_volume_valid[1]) {
        s_status.hardware_volume = true;
        s_software_volume = false;
    } else {
        s_software_volume = true;
        ESP_LOGW(TAG, "UAC2 Feature Unit volume unavailable; using PCM software gain");
    }

    s_backend = USB_AUDIO_BACKEND_UAC2;
    set_connected_locked(s_uac2_rate_count > 0);
    if (s_status.connected) xEventGroupSetBits(s_event_group, EVENT_DEVICE_READY);
}

static void probe_device_locked(void)
{
    uac_host_dev_info_t info = {0};
    if (uac_host_get_device_info(s_device, &info) != ESP_OK || info.type != UAC_STREAM_TX) {
        ESP_LOGE(TAG, "Failed to read UAC playback interface information");
        return;
    }

    s_status.vid = info.VID;
    s_status.pid = info.PID;
    copy_product(s_status.product, sizeof(s_status.product), info.iProduct);
    s_format_count = 0;
    ESP_LOGI(TAG, "UAC headphone opened addr=%u iface=%u VID=%04x PID=%04x product=%s alts=%u",
             info.addr, info.iface_num, info.VID, info.PID,
             s_status.product[0] ? s_status.product : "USB Audio", info.iface_alt_num);

    for (uint8_t alt = 1; alt <= info.iface_alt_num; ++alt) {
        uac_host_dev_alt_param_t param = {0};
        esp_err_t ret = uac_host_get_device_alt_param(s_device, alt, &param);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "UAC alt=%u query failed: %s", alt, esp_err_to_name(ret));
            continue;
        }
        if (param.sample_freq_type == 0) {
            ESP_LOGI(TAG, "UAC TX alt=%u PCM=%u ch=%u bit=%u rates=%" PRIu32 "..%" PRIu32,
                     alt, param.format, param.channels, param.bit_resolution,
                     param.sample_freq_lower, param.sample_freq_upper);
            // Record the application rates that fall inside a continuous range.
            const uint32_t candidates[] = {44100, 48000};
            for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
                if (candidates[i] >= param.sample_freq_lower &&
                    candidates[i] <= param.sample_freq_upper && param.format == 1) {
                    add_format(candidates[i], param.channels, param.bit_resolution);
                }
            }
        } else {
            for (uint8_t f = 0; f < param.sample_freq_type && f < CONFIG_UAC_FREQ_NUM_MAX; ++f) {
                ESP_LOGI(TAG, "UAC TX alt=%u PCM=%u ch=%u bit=%u rate=%" PRIu32,
                         alt, param.format, param.channels, param.bit_resolution,
                         param.sample_freq[f]);
                if (param.format == 1) {
                    add_format(param.sample_freq[f], param.channels, param.bit_resolution);
                }
            }
        }
    }
    const bool usable = s_format_count > 0;
    if (usable && s_backend != USB_AUDIO_BACKEND_UAC2) {
        s_backend = USB_AUDIO_BACKEND_UAC1;
        set_connected_locked(true);
        s_status.uac_version = 1;
        s_status.hardware_volume = false;
        s_software_volume = true;
        xEventGroupSetBits(s_event_group, EVENT_DEVICE_READY);
    } else if (!usable) {
        ESP_LOGW(TAG, "UAC playback interface has no usable PCM format");
    }
}

static void handle_driver_event(const usb_audio_event_t *message)
{
    if (message->driver.event != UAC_HOST_DRIVER_EVENT_TX_CONNECTED) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_accept_devices) {
        // deinit() is running. Opening here would push a device back into the
        // UAC driver's lists after the detach point, which makes
        // uac_host_uninstall() fail and leaks the USB Host stack.
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_device != NULL) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "Ignoring additional USB playback interface");
        return;
    }
    const uac_host_device_config_t config = {
        .addr = message->driver.addr,
        .iface_num = message->driver.iface_num,
        .buffer_size = USB_AUDIO_UAC1_PCM_BUFFER_SIZE,
        .buffer_threshold = USB_AUDIO_UAC1_PCM_THRESHOLD,
        .callback = device_callback,
        .callback_arg = NULL,
    };
    esp_err_t ret = uac_host_device_open(&config, &s_device);
    if (ret == ESP_OK) {
        probe_device_locked();
    } else {
        s_device = NULL;
        ESP_LOGE(TAG, "UAC playback interface open failed: %s", esp_err_to_name(ret));
    }
    xSemaphoreGive(s_lock);
}

static void handle_device_event(const usb_audio_event_t *message)
{
    if (message->device.event == UAC_HOST_DEVICE_EVENT_TX_DONE) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (message->device.handle != s_device) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (message->device.event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        ESP_LOGE(TAG, "UAC isochronous transfer error");
    }
    // Disconnects are handled synchronously in device_callback(); they never
    // reach this queue because the callback returns before enqueueing them.
    xSemaphoreGive(s_lock);
}

static void handle_uac2_driver_event(const usb_audio_event_t *message)
{
    if (message->uac2_driver.event != UAC2_HOST_DRIVER_EVENT_TX_CONNECTED) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_accept_devices || s_uac2_device != NULL) {
        xSemaphoreGive(s_lock);
        return;
    }
    const uac2_host_device_config_t config = {
        .addr = message->uac2_driver.addr,
        .iface_num = message->uac2_driver.iface_num,
        .buffer_size = USB_AUDIO_UAC2_PCM_BUFFER_SIZE,
        .buffer_threshold = USB_AUDIO_UAC2_PCM_THRESHOLD,
        .callback = uac2_device_callback,
        .callback_arg = NULL,
    };
    esp_err_t ret = uac2_host_device_open(&config, &s_uac2_device);
    if (ret == ESP_OK) {
        // Prefer UAC2 when both class clients saw the same composite device.
        // Leave an already-open UAC1 device idle; it remains the fallback for
        // genuinely UAC1-only hardware.
        probe_uac2_locked();
    } else {
        s_uac2_device = NULL;
        ESP_LOGW(TAG, "UAC2 playback interface open failed: %s", esp_err_to_name(ret));
    }
    xSemaphoreGive(s_lock);
}

static void handle_uac2_device_event(const usb_audio_event_t *message)
{
    if (message->uac2_device.event == UAC2_HOST_DEVICE_EVENT_TX_DONE) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (message->uac2_device.handle != s_uac2_device) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (message->uac2_device.event == UAC2_HOST_DEVICE_EVENT_DISCONNECTED) {
        uac2_host_device_handle_t detached = s_uac2_device;
        s_uac2_device = NULL;
        if (s_backend == USB_AUDIO_BACKEND_UAC2) {
            s_backend = s_device != NULL ? USB_AUDIO_BACKEND_UAC1 : USB_AUDIO_BACKEND_NONE;
            set_connected_locked(s_device != NULL);
            s_status.streaming = false;
            s_status.sample_rate = 0;
            s_status.uac_version = s_device != NULL ? 1 : 0;
            s_status.hardware_volume = false;
            s_software_volume = true;
            s_uac2_rate_count = 0;
            if (!s_status.connected) xEventGroupClearBits(s_event_group, EVENT_DEVICE_READY);
        }
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "UAC2 headphone disconnected");
        // The UAC2 driver explicitly requires close from a non-callback task.
        esp_err_t ret = uac2_host_device_close(detached);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "UAC2 close after disconnect failed: %s", esp_err_to_name(ret));
        }
        return;
    }
    if (message->uac2_device.event == UAC2_HOST_DEVICE_EVENT_TRANSFER_ERROR ||
        message->uac2_device.event == UAC2_HOST_DEVICE_EVENT_STREAM_ERROR) {
        ESP_LOGW(TAG, "UAC2 stream event=%d", message->uac2_device.event);
    }
    xSemaphoreGive(s_lock);
}

static void uac_event_task(void *arg)
{
    (void)arg;
    while (s_run) {
        esp_err_t ret = uac_host_handle_events(pdMS_TO_TICKS(20));
        // uac_host_uninstall() unblocks the client and waits for this call to
        // acknowledge its end-client flag. Exit only after that handshake;
        // stopping this task before uninstall would deadlock the uninstaller.
        if (s_uac_uninstalling && ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            break;
        }
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "UAC event handling returned %s", esp_err_to_name(ret));
        }
        usb_audio_event_t message;
        while (xQueueReceive(s_events, &message, 0) == pdTRUE) {
            if (message.type == USB_AUDIO_EVENT_DRIVER) handle_driver_event(&message);
            else if (message.type == USB_AUDIO_EVENT_DEVICE) handle_device_event(&message);
            else if (message.type == USB_AUDIO_EVENT_UAC2_DRIVER) handle_uac2_driver_event(&message);
            else handle_uac2_device_event(&message);
        }
    }
    xEventGroupSetBits(s_event_group, EVENT_UAC_TASK_DONE);
    s_uac_task = NULL;
    vTaskDelete(NULL);
}

static void host_event_task(void *arg)
{
    (void)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        // HS UAC2 OUT is serviced every 125 us. Keep this above the EPD
        // PARLIO/GDMA low/medium-priority activity and allow it to run while
        // external-memory cache maintenance is active.
        .intr_flags = ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM,
        .enum_filter_cb = NULL,
        .fifo_settings_custom = {0},
        .peripheral_map = 0,  // ESP32-P4 default is its High-Speed peripheral.
    };
    esp_err_t ret = usb_host_install(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(ret));
        // ESP_ERR_INVALID_STATE here means the library is already installed, i.e.
        // an earlier teardown never reached usb_host_uninstall(). Report what is
        // still attached so a leak can be told apart from a plain re-init.
        usb_host_lib_info_t leftover = {0};
        if (usb_host_lib_info(&leftover) == ESP_OK) {
            ESP_LOGE(TAG, "USB Host still installed: clients=%d devices=%d",
                     leftover.num_clients, leftover.num_devices);
        }
        xEventGroupSetBits(s_event_group, EVENT_HOST_FAILED | EVENT_HOST_TASK_DONE);
        s_host_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_host_lib_installed = true;
    ESP_LOGI(TAG, "ESP32-P4 USB Host installed on default HS peripheral");
    xEventGroupSetBits(s_event_group, EVENT_HOST_READY);

    bool no_clients = false;
    bool all_free = false;
    bool free_requested = false;
    while (!s_host_shutdown || !no_clients || !all_free) {
        uint32_t flags = 0;
        ret = usb_host_lib_handle_events(pdMS_TO_TICKS(50), &flags);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB Host event handling returned %s", esp_err_to_name(ret));
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) all_free = true;
        usb_host_lib_info_t info = {0};
        bool have_info = (usb_host_lib_info(&info) == ESP_OK);
        // num_clients is the current truth; the NO_CLIENTS flag is a one-shot
        // latch that also fires while the library idles before any client has
        // registered. Prefer the counter, and only fall back to the latch when
        // the library cannot be queried: a premature latch would let
        // usb_host_device_free_all() be called with a client still registered,
        // which fails and traps this loop forever.
        if (have_info) {
            no_clients = (info.num_clients == 0);
        } else if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            no_clients = true;
        }
        if (s_host_shutdown && no_clients && !free_requested) {
            esp_err_t free_ret = usb_host_device_free_all();
            if (free_ret == ESP_OK) free_requested = true;
            else if (free_ret == ESP_ERR_NOT_FINISHED) free_requested = true;
            else ESP_LOGW(TAG, "USB device free-all failed: %s", esp_err_to_name(free_ret));
        }
        if (s_host_shutdown && no_clients && free_requested) {
            if (have_info && info.num_devices == 0) all_free = true;
        }
    }
    // usb_host_uninstall() refuses to run while process_pending_flags,
    // lib_event_flags or flags.val are non-zero, and the first two are cleared in
    // exactly one place: inside usb_host_lib_handle_events(). Retrying the
    // uninstall alone therefore cannot change the outcome, it only re-reads the
    // same stale flag. That matters here because the loop above exits in the same
    // iteration it calls usb_host_device_free_all(), and that call raises a
    // processing request (hub teardown, then USBH freeing the device). Nothing
    // else in this task drains once the loop is done, so without a drain between
    // attempts every retry fails identically. The guard inside the uninstall sits
    // before any teardown work, so a failed call is safe to repeat.
    int attempt = 0;
    ret = ESP_ERR_INVALID_STATE;
    for (; attempt < 50 && ret != ESP_OK; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t drain_flags = 0;
        (void)usb_host_lib_handle_events(0, &drain_flags);
        ret = usb_host_uninstall();
    }
    if (ret == ESP_OK) {
        if (attempt > 1) {
            ESP_LOGI(TAG, "USB Host uninstall needed %d drain/retry rounds", attempt);
        }
        s_host_lib_installed = false;
    } else {
        usb_host_lib_info_t leftover = {0};
        (void)usb_host_lib_info(&leftover);
        ESP_LOGE(TAG, "USB Host uninstall failed: %s after %d drain/retry rounds "
                 "(clients=%d devices=%d)", esp_err_to_name(ret), attempt,
                 leftover.num_clients, leftover.num_devices);
    }
    xEventGroupSetBits(s_event_group, EVENT_HOST_TASK_DONE);
    s_host_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t usb_audio_output_init(void)
{
    if (s_initialized) return ESP_OK;
    if (s_teardown_failed) {
        // A previous deinit() could not remove its devices from the UAC driver,
        // leaving the USB Host stack installed with our client still registered.
        // Reinstalling is impossible and reusing the leftovers is unsafe, so
        // fail fast with an accurate reason instead of a bare
        // "USB Host install failed: ESP_ERR_INVALID_STATE".
        ESP_LOGE(TAG, "USB audio unusable: previous teardown left the USB Host stack installed");
        return ESP_ERR_INVALID_STATE;
    }
    log_heap("before-init");
    s_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(USB_AUDIO_EVENT_QUEUE_LEN, sizeof(usb_audio_event_t));
    s_event_group = xEventGroupCreate();
    if (s_lock == NULL || s_events == NULL || s_event_group == NULL) {
        usb_audio_output_deinit();
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    atomic_store_explicit(&s_connected_snapshot, false, memory_order_release);
    s_run = true;
    s_uac_uninstalling = false;
    s_host_shutdown = false;
    s_accept_devices = true;
    s_backend = USB_AUDIO_BACKEND_NONE;
    s_software_volume = true;

    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        host_event_task, "usb_host", USB_AUDIO_HOST_STACK, NULL,
        USB_AUDIO_HOST_TASK_PRIORITY, &s_host_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "USB Host task creation failed");
        usb_audio_output_deinit();
        return ESP_ERR_NO_MEM;
    }
    EventBits_t bits = xEventGroupWaitBits(s_event_group, EVENT_HOST_READY | EVENT_HOST_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(1500));
    if ((bits & EVENT_HOST_READY) == 0) {
        ESP_LOGE(TAG, "USB Host startup timed out or failed");
        usb_audio_output_deinit();
        return ESP_FAIL;
    }

    const uac_host_driver_config_t uac_config = {
        .create_background_task = false,
        .task_priority = 0,
        .stack_size = 0,
        .core_id = 0,
        .callback = driver_callback,
        .callback_arg = NULL,
    };
    esp_err_t ret = uac_host_install(&uac_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UAC class driver install failed: %s", esp_err_to_name(ret));
        usb_audio_output_deinit();
        return ret;
    }
    s_uac_installed = true;

    const uac2_host_driver_config_t uac2_config = {
        .create_background_task = true,
        // Isochronous URB completion/resubmission has a 125 us HS deadline.
        // Keep it above HLS/HTTP/decoder workers; PCM production is decoupled
        // by the internal 64 KiB reservoir.
        .task_priority = USB_AUDIO_UAC2_TASK_PRIORITY,
        .stack_size = USB_AUDIO_UAC2_STACK,
        .core_id = 0,
        .callback = uac2_driver_callback,
        .callback_arg = NULL,
    };
    ret = uac2_host_install(&uac2_config);
    if (ret == ESP_OK) {
        s_uac2_installed = true;
    } else {
        // UAC1 remains useful even if the optional UAC2 client cannot start.
        ESP_LOGW(TAG, "UAC2 class driver install failed: %s", esp_err_to_name(ret));
    }
    created = xTaskCreatePinnedToCoreWithCaps(
        uac_event_task, "usb_uac", USB_AUDIO_UAC_STACK, NULL,
        USB_AUDIO_UAC1_TASK_PRIORITY, &s_uac_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "UAC event task creation failed");
        usb_audio_output_deinit();
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    s_status.initialized = true;
    log_heap("after-init");
    return ESP_OK;
}

bool usb_audio_output_wait_connected(uint32_t timeout_ms)
{
    if (!s_initialized || s_event_group == NULL) return false;
    EventBits_t bits = xEventGroupWaitBits(s_event_group, EVENT_DEVICE_READY,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & EVENT_DEVICE_READY) != 0;
}

bool usb_audio_output_supports(uint32_t sample_rate, uint8_t channels, uint8_t bits)
{
    if (!s_initialized || s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool supported = s_status.connected &&
                     (s_backend == USB_AUDIO_BACKEND_UAC2
                          ? uac2_supports_locked(sample_rate, channels, bits)
                          : supports_locked(sample_rate, channels, bits));
    xSemaphoreGive(s_lock);
    return supported;
}

bool usb_audio_output_select_rate(uint32_t preferred_rate, uint32_t max_rate,
                                  uint8_t channels, uint8_t bits,
                                  uint32_t *selected_rate)
{
    if (selected_rate == NULL || !s_initialized || s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t best = 0;
    uint64_t best_distance = UINT64_MAX;
    if (s_backend == USB_AUDIO_BACKEND_UAC2 && uac2_has_alt_locked(channels, bits)) {
        for (size_t i = 0; i < s_uac2_rate_count; ++i) {
            const usb_audio_rate_range_t *range = &s_uac2_rates[i];
            uint32_t candidate = preferred_rate;
            if (candidate < range->min) candidate = range->min;
            if (candidate > range->max) candidate = range->max;
            if (range->res != 0) {
                uint32_t steps = (candidate - range->min + range->res / 2) / range->res;
                candidate = range->min + steps * range->res;
                if (candidate > range->max) candidate = range->max;
            }
            if (max_rate != 0 && candidate > max_rate) {
                candidate = max_rate < range->min ? 0 : max_rate;
            }
            if (candidate == 0 || !rate_in_uac2_range(candidate, range)) continue;
            uint64_t distance = candidate > preferred_rate
                                    ? candidate - preferred_rate
                                    : preferred_rate - candidate;
            if (distance < best_distance) {
                best = candidate;
                best_distance = distance;
            }
        }
    } else {
        for (size_t i = 0; i < s_format_count; ++i) {
            const usb_audio_format_t *format = &s_formats[i];
            if (format->channels != channels || format->bits != bits ||
                format->sample_rate == 0 || (max_rate != 0 && format->sample_rate > max_rate)) {
                continue;
            }
            uint64_t distance = format->sample_rate > preferred_rate
                                    ? format->sample_rate - preferred_rate
                                    : preferred_rate - format->sample_rate;
            if (distance < best_distance) {
                best = format->sample_rate;
                best_distance = distance;
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (best == 0) return false;
    *selected_rate = best;
    return true;
}

bool usb_audio_output_select_format(uint32_t source_rate, uint8_t source_channels,
                                    uint8_t source_bits, uint32_t max_rate,
                                    uint32_t *selected_rate, uint8_t *selected_channels,
                                    uint8_t *selected_bits)
{
    if (selected_rate == NULL || selected_channels == NULL || selected_bits == NULL) {
        return false;
    }
    const uint8_t bit_order[] = {source_bits, 32, 24, 16};
    const uint8_t channel_order[] = {source_channels, 2, 1};
    for (size_t c = 0; c < sizeof(channel_order); ++c) {
        if (channel_order[c] == 0 || (c > 0 && channel_order[c] == source_channels)) continue;
        for (size_t b = 0; b < sizeof(bit_order); ++b) {
            if (bit_order[b] == 0) continue;
            bool duplicate = false;
            for (size_t earlier = 0; earlier < b; ++earlier) {
                if (bit_order[earlier] == bit_order[b]) duplicate = true;
            }
            if (duplicate) continue;
            uint32_t rate = 0;
            if (usb_audio_output_select_rate(source_rate, max_rate,
                                             channel_order[c], bit_order[b], &rate)) {
                *selected_rate = rate;
                *selected_channels = channel_order[c];
                *selected_bits = bit_order[b];
                return true;
            }
        }
    }
    return false;
}

esp_err_t usb_audio_output_start(uint32_t sample_rate, uint8_t channels, uint8_t bits)
{
    if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if ((s_backend == USB_AUDIO_BACKEND_UAC2 ? s_uac2_device == NULL : s_device == NULL) ||
        !s_status.connected) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const bool supported = s_backend == USB_AUDIO_BACKEND_UAC2
                               ? uac2_supports_locked(sample_rate, channels, bits)
                               : supports_locked(sample_rate, channels, bits);
    if (!supported) {
        ESP_LOGW(TAG, "UAC format unsupported: %" PRIu32 " Hz, %u ch, %u bit",
                 sample_rate, channels, bits);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_status.streaming && s_status.sample_rate == sample_rate &&
        s_status.channels == channels && s_status.bits == bits) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_status.streaming) {
        if (s_backend == USB_AUDIO_BACKEND_UAC2 && s_uac2_device != NULL) {
            (void)uac2_host_device_stop(s_uac2_device);
        } else if (s_device != NULL) {
            (void)uac_host_device_stop(s_device);
        }
        s_status.streaming = false;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_backend == USB_AUDIO_BACKEND_UAC2 && s_uac2_device != NULL) {
        const uac2_host_stream_config_t stream = {
            .channels = channels, .bit_resolution = bits,
            .sample_freq = sample_rate, .flags = 0,
        };
        ret = uac2_host_device_start(s_uac2_device, &stream);
    } else if (s_device != NULL) {
        const uac_host_stream_config_t stream = {
            .channels = channels, .bit_resolution = bits,
            .sample_freq = sample_rate, .flags = 0,
        };
        ret = uac_host_device_start(s_device, &stream);
    }
    if (ret == ESP_OK) {
        s_status.streaming = true;
        s_status.sample_rate = sample_rate;
        s_status.channels = channels;
        s_status.bits = bits;
        ESP_LOGI(TAG, "UAC%u stream started: %" PRIu32 " Hz, %u ch, %u bit",
                 s_status.uac_version, sample_rate, channels, bits);
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t usb_audio_output_stop(void)
{
    if (!s_initialized || s_lock == NULL) return ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (s_status.streaming) {
        if (s_backend == USB_AUDIO_BACKEND_UAC2 && s_uac2_device != NULL) {
            ret = uac2_host_device_stop(s_uac2_device);
        } else if (s_device != NULL) {
            ret = uac_host_device_stop(s_device);
        }
    }
    s_status.streaming = false;
    s_status.sample_rate = 0;
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t usb_audio_output_write(const uint8_t *data, size_t size, uint32_t timeout_ms)
{
    if (data == NULL || size == 0) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_status.streaming && s_backend == USB_AUDIO_BACKEND_UAC2 && s_uac2_device != NULL) {
        ret = uac2_host_device_write(s_uac2_device, data, (uint32_t)size, timeout_ms);
    } else if (s_status.streaming && s_device != NULL) {
        ret = uac_host_device_write(s_device, (uint8_t *)data, (uint32_t)size,
                                    pdMS_TO_TICKS(timeout_ms));
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t usb_audio_output_set_volume(uint8_t volume)
{
    if (volume > 100) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (s_backend == USB_AUDIO_BACKEND_UAC2 && s_uac2_device != NULL &&
        !s_software_volume) {
        if (volume == 0) {
            ret = uac2_host_device_set_mute(s_uac2_device, 0, true);
        } else {
            const float effective = ((float)volume *
                                     (float)CONFIG_MYPAPER_USB_AUDIO_MAX_VOLUME) / 10000.0f;
            int32_t requested = (int32_t)lrintf(20.0f * log10f(effective) * 256.0f);
            ret = ESP_OK;
            for (uint8_t channel = 1; channel <= 2 && ret == ESP_OK; ++channel) {
                const uac2_volume_range_t *range = &s_uac2_volume[channel - 1];
                int32_t value = requested;
                if (value < range->min) value = range->min;
                if (value > range->max) value = range->max;
                int32_t res = range->res > 0 ? range->res : 1;
                value = range->min + ((value - range->min + res / 2) / res) * res;
                if (value > range->max) value = range->max;
                ret = uac2_host_device_set_volume(s_uac2_device, channel,
                                                   (int16_t)value);
            }
            // Program attenuation before unmuting so a newly connected DAC
            // cannot emit a short full-volume burst.
            if (ret == ESP_OK) {
                ret = uac2_host_device_set_mute(s_uac2_device, 0, false);
            }
        }
        if (ret != ESP_OK) {
            s_software_volume = true;
            s_status.hardware_volume = false;
            ESP_LOGW(TAG, "UAC2 hardware volume failed: %s; switching to PCM gain",
                     esp_err_to_name(ret));
            ret = ESP_OK;
        }
    } else if (s_backend == USB_AUDIO_BACKEND_UAC1 && s_device != NULL) {
        // UAC1 hardware controls have already proved unreliable on some tested
        // headsets. Keep the established software-volume path.
        ret = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

bool usb_audio_output_uses_software_volume(void)
{
    if (!s_initialized || s_lock == NULL) return true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool result = s_software_volume;
    xSemaphoreGive(s_lock);
    return result;
}

bool usb_audio_output_is_connected(void)
{
    return atomic_load_explicit(&s_connected_snapshot, memory_order_acquire);
}

void usb_audio_output_get_status(usb_audio_output_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_lock);
}

void usb_audio_output_deinit(void)
{
    if (s_teardown_failed) {
        // The primitives are deliberately kept alive after a failed teardown so
        // the still-running host task never sees freed memory. Nothing left to
        // release here, and retrying uac_host_uninstall() cannot succeed.
        ESP_LOGW(TAG, "Deinit skipped: teardown already failed for this boot");
        return;
    }
    ESP_LOGI(TAG, "Deinit: detach device begin initialized=%d installed=%d",
             s_initialized, s_uac_installed);
    uac_host_device_handle_t device = NULL;
    uac2_host_device_handle_t uac2_device = NULL;
    bool streaming = false;
    usb_audio_backend_t active_backend = USB_AUDIO_BACKEND_NONE;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        device = s_device;
        uac2_device = s_uac2_device;
        streaming = s_status.streaming;
        active_backend = s_backend;
        // Detach the application-visible handle before potentially blocking
        // driver operations. The UAC event task remains alive for completion.
        // Stop accepting new devices from here on: a queued TX_CONNECTED
        // handled after this point would reopen a device that the uninstall
        // below is about to require absent.
        s_accept_devices = false;
        s_device = NULL;
        s_uac2_device = NULL;
        s_backend = USB_AUDIO_BACKEND_NONE;
        memset(&s_status, 0, sizeof(s_status));
        atomic_store_explicit(&s_connected_snapshot, false, memory_order_release);
        s_format_count = 0;
        if (s_event_group != NULL) xEventGroupClearBits(s_event_group, EVENT_DEVICE_READY);
        xSemaphoreGive(s_lock);
    }

    if (device != NULL) {
        if (streaming && active_backend == USB_AUDIO_BACKEND_UAC1) {
            esp_err_t stop_ret = uac_host_device_stop(device);
            if (stop_ret != ESP_OK) {
                ESP_LOGW(TAG, "UAC device stop during deinit failed: %s",
                         esp_err_to_name(stop_ret));
            }
        }
        esp_err_t close_ret = uac_host_device_close(device);
        // Note: ESP_OK here does not guarantee the interface was removed. The
        // component returns ESP_OK without deleting anything when
        // FLAG_INTERFACE_WAIT_USER_DELETE is still set on the interface, which
        // happens when its disconnect handler bailed out on a failed close.
        ESP_LOGI(TAG, "Deinit: device close returned %s", esp_err_to_name(close_ret));
    }

    if (uac2_device != NULL) {
        if (streaming && active_backend == USB_AUDIO_BACKEND_UAC2) {
            esp_err_t stop_ret = uac2_host_device_stop(uac2_device);
            if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "UAC2 device stop during deinit failed: %s",
                         esp_err_to_name(stop_ret));
            }
        }
        esp_err_t close_ret = uac2_host_device_close(uac2_device);
        ESP_LOGI(TAG, "Deinit: UAC2 device close returned %s", esp_err_to_name(close_ret));
    }

    bool uac2_torn_down = true;
    if (s_uac2_installed) {
        ESP_LOGI(TAG, "Deinit: UAC2 driver uninstall begin");
        esp_err_t ret = uac2_host_uninstall();
        if (ret == ESP_OK) s_uac2_installed = false;
        else {
            uac2_torn_down = false;
            ESP_LOGE(TAG, "UAC2 uninstall failed: %s", esp_err_to_name(ret));
        }
    }

    // IMPORTANT: keep uac_event_task alive while uninstall runs. The UAC
    // component waits for uac_host_handle_events() to acknowledge its unblock.
    // uac_host_uninstall() is the only honest confirmation that no device or
    // interface is left behind: it refuses to run while either list is non-empty.
    bool uac_torn_down = true;
    if (s_uac_installed) {
        s_uac_uninstalling = true;
        ESP_LOGI(TAG, "Deinit: UAC driver uninstall begin");
        esp_err_t ret = uac_host_uninstall();
        if (ret == ESP_OK) {
            s_uac_installed = false;
        } else {
            uac_torn_down = false;
            ESP_LOGE(TAG, "UAC uninstall failed: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "Deinit: UAC driver uninstall complete ret=%s", esp_err_to_name(ret));
    }

    if (!uac_torn_down || !uac2_torn_down) {
        // Our client is still registered, so USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS
        // will never be raised and host_event_task can never reach
        // usb_host_uninstall(). Leave the host task running and keep every
        // primitive it references alive: freeing them here would be a
        // use-after-free, and the next init() cannot reinstall the host anyway.
        s_run = false;
        s_teardown_failed = true;
        s_initialized = false;
        ESP_LOGE(TAG, "USB audio disabled for this boot: a device or interface could not "
                      "be removed from the UAC driver (reboot to recover)");
        log_heap("after-deinit-incomplete");
        return;
    }

    s_run = false;
    if (s_uac_task != NULL && s_event_group != NULL) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, EVENT_UAC_TASK_DONE,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if ((bits & EVENT_UAC_TASK_DONE) == 0) {
            ESP_LOGW(TAG, "UAC event task exit timed out");
        }
    }

    s_host_shutdown = true;
    ESP_LOGI(TAG, "Deinit: USB Host shutdown requested");
    if (s_host_task != NULL && s_event_group != NULL) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, EVENT_HOST_TASK_DONE,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        if ((bits & EVENT_HOST_TASK_DONE) == 0) {
            ESP_LOGW(TAG, "USB Host task exit timed out");
        }
    }
    // The host task exits even when its usb_host_uninstall() call failed, so the
    // task's departure says nothing about the library. Ask the library itself.
    bool host_torn_down = !s_host_lib_installed;

    if (!host_torn_down) {
        // Either the host task never stopped, or it stopped with the library
        // still installed because usb_host_uninstall() failed. Both leave the
        // library unusable and the task may still be touching s_event_group, so
        // keep every primitive allocated: freeing them would be a use-after-free,
        // and the next init() could not reinstall the host anyway.
        s_teardown_failed = true;
        s_initialized = false;
        ESP_LOGE(TAG, "USB Host library could not be uninstalled; USB audio disabled for "
                      "this boot (reboot to recover)");
        log_heap("after-deinit-incomplete");
        return;
    }

    ESP_LOGI(TAG, "Deinit: USB Host task complete");
    s_initialized = false;
    if (s_events != NULL) {
        vQueueDelete(s_events);
        s_events = NULL;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }
    s_uac_uninstalling = false;
    s_teardown_failed = false;
    log_heap("after-deinit");
}

#else

esp_err_t usb_audio_output_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t usb_audio_output_stop(void) { return ESP_OK; }
esp_err_t usb_audio_output_start(uint32_t r, uint8_t c, uint8_t b)
{ (void)r; (void)c; (void)b; return ESP_ERR_NOT_SUPPORTED; }
bool usb_audio_output_supports(uint32_t r, uint8_t c, uint8_t b)
{ (void)r; (void)c; (void)b; return false; }
bool usb_audio_output_select_rate(uint32_t p, uint32_t m, uint8_t c, uint8_t b,
                                  uint32_t *selected)
{ (void)p; (void)m; (void)c; (void)b; (void)selected; return false; }
bool usb_audio_output_select_format(uint32_t r, uint8_t c, uint8_t b, uint32_t m,
                                    uint32_t *sr, uint8_t *sc, uint8_t *sb)
{ (void)r; (void)c; (void)b; (void)m; (void)sr; (void)sc; (void)sb; return false; }
bool usb_audio_output_wait_connected(uint32_t t) { (void)t; return false; }
esp_err_t usb_audio_output_write(const uint8_t *d, size_t s, uint32_t t)
{ (void)d; (void)s; (void)t; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t usb_audio_output_set_volume(uint8_t v) { (void)v; return ESP_ERR_NOT_SUPPORTED; }
bool usb_audio_output_uses_software_volume(void) { return true; }
bool usb_audio_output_is_connected(void) { return false; }
void usb_audio_output_get_status(usb_audio_output_status_t *status)
{ if (status != NULL) memset(status, 0, sizeof(*status)); }
void usb_audio_output_deinit(void) {}

#endif
