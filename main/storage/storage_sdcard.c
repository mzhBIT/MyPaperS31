#include "storage_sdcard.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sd_protocol_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#define STORAGE_SDCARD_PIN_CLK 43
#define STORAGE_SDCARD_PIN_CMD 44
#define STORAGE_SDCARD_PIN_D0  39
#define STORAGE_SDCARD_PIN_D1  40
#define STORAGE_SDCARD_PIN_D2  41
#define STORAGE_SDCARD_PIN_D3  42
#define STORAGE_SDCARD_MAX_PATH_LEN 256
#define STORAGE_USB_WRITE_CACHE_BYTES (32 * 1024)
#define STORAGE_USB_WRITE_IDLE_FLUSH_US 10000
#define STORAGE_USB_WRITE_SLOT_COUNT 2
#define STORAGE_USB_WRITE_IDLE_BIT BIT0
#define STORAGE_USB_STATS_INTERVAL_US (10 * 1000 * 1000)

static const char *TAG = "storage_sdcard";

static sdmmc_card_t *s_card;
static bool s_usb_medium_initialized;
static sdmmc_host_t s_usb_host;
static esp_err_t (*s_usb_do_transaction)(int slot, sdmmc_command_t *cmdinfo);
static volatile bool s_usb_stats_enabled;
static int64_t s_usb_stats_last_report_us;
static portMUX_TYPE s_usb_stats_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint8_t *data;
    uint32_t lba;
    size_t bytes;
} storage_sdcard_write_slot_t;

static storage_sdcard_write_slot_t s_usb_write_slots[STORAGE_USB_WRITE_SLOT_COUNT];
static int s_usb_write_active_slot = -1;
static int64_t s_usb_write_cache_last_append_us;
static SemaphoreHandle_t s_usb_write_gate;
static SemaphoreHandle_t s_usb_write_state_mutex;
static SemaphoreHandle_t s_usb_write_free_slots;
static SemaphoreHandle_t s_usb_write_task_done;
static QueueHandle_t s_usb_write_queue;
static EventGroupHandle_t s_usb_write_events;
static TaskHandle_t s_usb_write_task;
static uint32_t s_usb_write_pending;
static esp_err_t s_usb_write_async_error;
static bool s_usb_write_fake_next_status;

typedef struct {
    uint64_t bytes;
    uint64_t busy_us;
    uint32_t commands;
    uint32_t max_us;
    uint32_t sizes[5];
} storage_sdcard_io_stats_t;

static storage_sdcard_io_stats_t s_usb_read_stats;
static storage_sdcard_io_stats_t s_usb_write_stats;

static esp_err_t storage_sdcard_queue_active_slot_locked(void)
{
    if (s_usb_write_active_slot < 0) return ESP_OK;
    storage_sdcard_write_slot_t *slot = &s_usb_write_slots[s_usb_write_active_slot];
    if (slot->bytes == 0) return ESP_OK;

    const int queued_slot = s_usb_write_active_slot;
    s_usb_write_active_slot = -1;
    s_usb_write_pending++;
    xEventGroupClearBits(s_usb_write_events, STORAGE_USB_WRITE_IDLE_BIT);
    if (xQueueSend(s_usb_write_queue, &queued_slot, 0) != pdTRUE) {
        s_usb_write_pending--;
        s_usb_write_active_slot = queued_slot;
        if (s_usb_write_pending == 0) {
            xEventGroupSetBits(s_usb_write_events, STORAGE_USB_WRITE_IDLE_BIT);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void storage_sdcard_usb_writer_task(void *arg)
{
    (void)arg;
    while (true) {
        int slot_index = -1;
        if (xQueueReceive(s_usb_write_queue, &slot_index, portMAX_DELAY) != pdTRUE) continue;
        if (slot_index < 0) break;

        storage_sdcard_write_slot_t *slot = &s_usb_write_slots[slot_index];
        const size_t sector_count = slot->bytes / s_card->csd.sector_size;
        const esp_err_t ret = sdmmc_write_sectors(s_card, slot->data, slot->lba, sector_count);

        xSemaphoreTake(s_usb_write_state_mutex, portMAX_DELAY);
        if (ret != ESP_OK && s_usb_write_async_error == ESP_OK) {
            s_usb_write_async_error = ret;
            ESP_LOGE(TAG, "USB MSC async write failed lba=%lu sectors=%lu: %s",
                     (unsigned long)slot->lba, (unsigned long)sector_count,
                     esp_err_to_name(ret));
        }
        slot->bytes = 0;
        if (s_usb_write_pending > 0) s_usb_write_pending--;
        if (s_usb_write_pending == 0) {
            xEventGroupSetBits(s_usb_write_events, STORAGE_USB_WRITE_IDLE_BIT);
        }
        xSemaphoreGive(s_usb_write_state_mutex);
        xSemaphoreGive(s_usb_write_free_slots);
    }
    xSemaphoreGive(s_usb_write_task_done);
    vTaskDelete(NULL);
}

static esp_err_t storage_sdcard_flush_under_gate(void)
{
    xSemaphoreTake(s_usb_write_state_mutex, portMAX_DELAY);
    esp_err_t ret = storage_sdcard_queue_active_slot_locked();
    xSemaphoreGive(s_usb_write_state_mutex);
    if (ret != ESP_OK) return ret;

    if ((xEventGroupWaitBits(s_usb_write_events, STORAGE_USB_WRITE_IDLE_BIT,
                             pdFALSE, pdTRUE, pdMS_TO_TICKS(5000)) &
         STORAGE_USB_WRITE_IDLE_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_usb_write_state_mutex, portMAX_DELAY);
    ret = s_usb_write_async_error;
    xSemaphoreGive(s_usb_write_state_mutex);
    return ret;
}

esp_err_t storage_sdcard_flush_usb_write_cache(void)
{
    if (s_usb_write_task == NULL || s_usb_write_gate == NULL) return ESP_OK;
    if (xSemaphoreTake(s_usb_write_gate, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ret = storage_sdcard_flush_under_gate();
    xSemaphoreGive(s_usb_write_gate);
    return ret;
}

static esp_err_t storage_sdcard_cache_single_write(sdmmc_command_t *cmdinfo)
{
    if (s_usb_write_task == NULL || s_usb_write_gate == NULL || s_card == NULL ||
        cmdinfo->datalen != s_card->csd.sector_size ||
        cmdinfo->blklen != s_card->csd.sector_size) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint32_t lba = (s_card->ocr & SD_OCR_SDHC_CAP)
        ? cmdinfo->arg : cmdinfo->arg / s_card->csd.sector_size;
    if (xSemaphoreTake(s_usb_write_gate, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    while (ret == ESP_OK) {
        xSemaphoreTake(s_usb_write_state_mutex, portMAX_DELAY);
        if (s_usb_write_async_error != ESP_OK) {
            ret = s_usb_write_async_error;
            xSemaphoreGive(s_usb_write_state_mutex);
            break;
        }

        if (s_usb_write_active_slot >= 0) {
            storage_sdcard_write_slot_t *active =
                &s_usb_write_slots[s_usb_write_active_slot];
            const uint32_t next_lba = active->lba +
                (uint32_t)(active->bytes / s_card->csd.sector_size);
            if (active->bytes != 0 &&
                (lba != next_lba ||
                 active->bytes + cmdinfo->datalen > STORAGE_USB_WRITE_CACHE_BYTES)) {
                ret = storage_sdcard_queue_active_slot_locked();
                xSemaphoreGive(s_usb_write_state_mutex);
                continue;
            }
        }

        if (s_usb_write_active_slot < 0) {
            xSemaphoreGive(s_usb_write_state_mutex);
            if (xSemaphoreTake(s_usb_write_free_slots, pdMS_TO_TICKS(5000)) != pdTRUE) {
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            xSemaphoreTake(s_usb_write_state_mutex, portMAX_DELAY);
            for (int i = 0; i < STORAGE_USB_WRITE_SLOT_COUNT; ++i) {
                if (s_usb_write_slots[i].bytes == 0) {
                    s_usb_write_active_slot = i;
                    break;
                }
            }
            if (s_usb_write_active_slot < 0) {
                xSemaphoreGive(s_usb_write_free_slots);
                ret = ESP_ERR_INVALID_STATE;
                xSemaphoreGive(s_usb_write_state_mutex);
                break;
            }
        }

        storage_sdcard_write_slot_t *active =
            &s_usb_write_slots[s_usb_write_active_slot];
        if (active->bytes == 0) active->lba = lba;
        memcpy(active->data + active->bytes, cmdinfo->data, cmdinfo->datalen);
        active->bytes += cmdinfo->datalen;
        s_usb_write_cache_last_append_us = esp_timer_get_time();
        if (active->bytes == STORAGE_USB_WRITE_CACHE_BYTES) {
            ret = storage_sdcard_queue_active_slot_locked();
        }
        xSemaphoreGive(s_usb_write_state_mutex);
        break;
    }
    if (ret == ESP_OK) {
        cmdinfo->error = ESP_OK;
        memset(cmdinfo->response, 0, sizeof(cmdinfo->response));
        s_usb_write_fake_next_status = true;
    }
    xSemaphoreGive(s_usb_write_gate);
    return ret;
}

static size_t storage_sdcard_size_bucket(size_t bytes)
{
    if (bytes <= 512) return 0;
    if (bytes <= 4096) return 1;
    if (bytes <= 16384) return 2;
    if (bytes <= 32768) return 3;
    return 4;
}

static esp_err_t storage_sdcard_usb_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    // A cache flush deliberately re-enters the normal SDMMC API. Let that
    // task reach the real host callback instead of recursively caching it.
    const bool is_flush_io = s_usb_write_task == xTaskGetCurrentTaskHandle();
    if (!is_flush_io && s_usb_stats_enabled && cmdinfo != NULL) {
        if (cmdinfo->opcode == MMC_SEND_STATUS && s_usb_write_fake_next_status) {
            s_usb_write_fake_next_status = false;
            cmdinfo->error = ESP_OK;
            memset(cmdinfo->response, 0, sizeof(cmdinfo->response));
            cmdinfo->response[0] = MMC_R1_READY_FOR_DATA |
                (MMC_R1_CURRENT_STATE_TRAN << MMC_R1_CURRENT_STATE_POS);
            return ESP_OK;
        }
        if (cmdinfo->opcode == MMC_WRITE_BLOCK_SINGLE && cmdinfo->data != NULL) {
            const esp_err_t cached = storage_sdcard_cache_single_write(cmdinfo);
            if (cached != ESP_ERR_NOT_SUPPORTED) return cached;
        }
        // Reads and externally-issued multi-block writes must observe all
        // earlier acknowledged writes.
        if (cmdinfo->opcode == MMC_READ_BLOCK_SINGLE ||
            cmdinfo->opcode == MMC_READ_BLOCK_MULTIPLE ||
            cmdinfo->opcode == MMC_WRITE_BLOCK_MULTIPLE) {
            const esp_err_t flush_ret = storage_sdcard_flush_usb_write_cache();
            if (flush_ret != ESP_OK) return flush_ret;
        }
    }

    const int64_t begin_us = esp_timer_get_time();
    const esp_err_t ret = s_usb_do_transaction(slot, cmdinfo);
    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - begin_us);

    if (cmdinfo != NULL && cmdinfo->data != NULL &&
        cmdinfo->datalen != 0 && SCF_CMD(cmdinfo->flags) == SCF_CMD_ADTC) {
        portENTER_CRITICAL(&s_usb_stats_mux);
        if (s_usb_stats_enabled) {
            storage_sdcard_io_stats_t *stats =
                (cmdinfo->flags & SCF_CMD_READ) ? &s_usb_read_stats : &s_usb_write_stats;
            const size_t bucket = storage_sdcard_size_bucket(cmdinfo->datalen);
            stats->bytes += cmdinfo->datalen;
            stats->busy_us += elapsed_us;
            stats->commands++;
            if (elapsed_us > stats->max_us) stats->max_us = elapsed_us;
            stats->sizes[bucket]++;
        }
        portEXIT_CRITICAL(&s_usb_stats_mux);
    }
    return ret;
}

static void storage_sdcard_log_one_io(const char *direction,
                                      const storage_sdcard_io_stats_t *stats,
                                      int64_t window_us)
{
    if (stats->commands == 0 || window_us <= 0) return;
    const double window_kib_s = (double)stats->bytes * 1000000.0 /
                                ((double)window_us * 1024.0);
    const double busy_kib_s = stats->busy_us != 0
        ? (double)stats->bytes * 1000000.0 / ((double)stats->busy_us * 1024.0)
        : 0.0;
    ESP_LOGI(TAG,
             "USB MSC %s: %.1f KiB/s wall, %.1f KiB/s SD-busy, bytes=%llu cmds=%lu avg=%lu us max=%lu us sizes[512/4K/16K/32K/>]=%lu/%lu/%lu/%lu/%lu",
             direction, window_kib_s, busy_kib_s,
             (unsigned long long)stats->bytes,
             (unsigned long)stats->commands,
             (unsigned long)(stats->busy_us / stats->commands),
             (unsigned long)stats->max_us,
             (unsigned long)stats->sizes[0], (unsigned long)stats->sizes[1],
             (unsigned long)stats->sizes[2], (unsigned long)stats->sizes[3],
             (unsigned long)stats->sizes[4]);
}

#if CONFIG_STORAGE_SD_PWR_CTRL_LDO_INTERNAL_IO
static sd_pwr_ctrl_handle_t s_pwr_ctrl_handle;
#endif

static esp_err_t storage_sdcard_init_power(sdmmc_host_t *host)
{
#if CONFIG_STORAGE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_STORAGE_SD_PWR_CTRL_LDO_IO_ID,
    };

    esp_log_level_t old_ldo_log_level = esp_log_level_get("ldo");
    esp_log_level_set("ldo", ESP_LOG_ERROR);
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl_handle);
    esp_log_level_set("ldo", old_ldo_log_level);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SD card LDO power control: %s", esp_err_to_name(ret));
        return ret;
    }

    host->pwr_ctrl_handle = s_pwr_ctrl_handle;
#endif
    return ESP_OK;
}

static void storage_sdcard_deinit_power(void)
{
#if CONFIG_STORAGE_SD_PWR_CTRL_LDO_INTERNAL_IO
    if (s_pwr_ctrl_handle != NULL) {
        esp_err_t ret = sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to release SD card LDO power control: %s", esp_err_to_name(ret));
        }
        s_pwr_ctrl_handle = NULL;
    }
#endif
}

static void storage_sdcard_log_root_files(void)
{
    DIR *dir = opendir(STORAGE_SDCARD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "Failed to open SD card root directory");
        return;
    }

    ESP_LOGI(TAG, "SD card root directory:");

    bool found_entry = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char path[STORAGE_SDCARD_MAX_PATH_LEN];
        int written = snprintf(path, sizeof(path), "%s/%s", STORAGE_SDCARD_MOUNT_POINT, entry->d_name);
        if (written < 0 || (size_t) written >= sizeof(path)) {
            ESP_LOGW(TAG, "  ? %s (path too long)", entry->d_name);
            continue;
        }

        struct stat st;
        if (stat(path, &st) == 0) {
            ESP_LOGI(TAG, "  %c %-32s %ld bytes",
                     S_ISDIR(st.st_mode) ? 'd' : '-',
                     entry->d_name,
                     (long) st.st_size);
        } else {
            ESP_LOGI(TAG, "  ? %s", entry->d_name);
        }
        found_entry = true;
    }

    if (!found_entry) {
        ESP_LOGI(TAG, "  <empty>");
    }

    closedir(dir);
}

static void storage_sdcard_configure_bus(sdmmc_host_t *host, sdmmc_slot_config_t *slot_config)
{
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->slot = SDMMC_HOST_SLOT_0;
    host->max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host->io_voltage = 3.3f;

    *slot_config = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config->width = 4;
    slot_config->clk = STORAGE_SDCARD_PIN_CLK;
    slot_config->cmd = STORAGE_SDCARD_PIN_CMD;
    slot_config->d0 = STORAGE_SDCARD_PIN_D0;
    slot_config->d1 = STORAGE_SDCARD_PIN_D1;
    slot_config->d2 = STORAGE_SDCARD_PIN_D2;
    slot_config->d3 = STORAGE_SDCARD_PIN_D3;
    slot_config->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

esp_err_t storage_sdcard_mount(void)
{
    if (s_card != NULL) {
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host;
    sdmmc_slot_config_t slot_config;
    storage_sdcard_configure_bus(&host, &slot_config);

    esp_err_t ret = storage_sdcard_init_power(&host);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Mounting SD card at %s", STORAGE_SDCARD_MOUNT_POINT);
    ret = esp_vfs_fat_sdmmc_mount(STORAGE_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        storage_sdcard_deinit_power();
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    // storage_sdcard_log_root_files();
    return ESP_OK;
}

esp_err_t storage_sdcard_init_usb_medium(void)
{
    if (s_card != NULL) {
        return s_usb_medium_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    sdmmc_slot_config_t slot_config;
    storage_sdcard_configure_bus(&s_usb_host, &slot_config);

    // Interpose only for the dedicated raw MSC medium. The wrapper adds two
    // timestamps per SD command and reports aggregates outside this callback.
    s_usb_do_transaction = s_usb_host.do_transaction;
    s_usb_host.do_transaction = storage_sdcard_usb_do_transaction;

    esp_err_t ret = storage_sdcard_init_power(&s_usb_host);
    if (ret != ESP_OK) return ret;

    s_card = calloc(1, sizeof(*s_card));
    if (s_card == NULL) {
        storage_sdcard_deinit_power();
        return ESP_ERR_NO_MEM;
    }

    ret = (*s_usb_host.init)();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB MSC SD host: %s", esp_err_to_name(ret));
        free(s_card);
        s_card = NULL;
        storage_sdcard_deinit_power();
        return ret;
    }
    s_usb_medium_initialized = true;

    ret = sdmmc_host_init_slot(s_usb_host.slot, &slot_config);
    if (ret != ESP_OK) goto fail;

    ret = sdmmc_card_init(&s_usb_host, s_card);
    if (ret != ESP_OK) goto fail;

    s_usb_write_gate = xSemaphoreCreateMutex();
    s_usb_write_state_mutex = xSemaphoreCreateMutex();
    s_usb_write_free_slots = xSemaphoreCreateCounting(STORAGE_USB_WRITE_SLOT_COUNT,
                                                       STORAGE_USB_WRITE_SLOT_COUNT);
    s_usb_write_task_done = xSemaphoreCreateBinary();
    s_usb_write_queue = xQueueCreate(STORAGE_USB_WRITE_SLOT_COUNT, sizeof(int));
    s_usb_write_events = xEventGroupCreate();
    for (int i = 0; i < STORAGE_USB_WRITE_SLOT_COUNT; ++i) {
        s_usb_write_slots[i].data = heap_caps_aligned_alloc(
            64, STORAGE_USB_WRITE_CACHE_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        s_usb_write_slots[i].bytes = 0;
    }

    const bool cache_resources_ready =
        s_usb_write_gate != NULL && s_usb_write_state_mutex != NULL &&
        s_usb_write_free_slots != NULL && s_usb_write_task_done != NULL &&
        s_usb_write_queue != NULL && s_usb_write_events != NULL &&
        s_usb_write_slots[0].data != NULL && s_usb_write_slots[1].data != NULL;
    BaseType_t task_created = pdFAIL;
    if (cache_resources_ready) {
        xEventGroupSetBits(s_usb_write_events, STORAGE_USB_WRITE_IDLE_BIT);
        s_usb_write_active_slot = -1;
        s_usb_write_pending = 0;
        s_usb_write_async_error = ESP_OK;
        s_usb_write_fake_next_status = false;
        s_usb_write_cache_last_append_us = 0;
        task_created = xTaskCreatePinnedToCore(
            storage_sdcard_usb_writer_task, "usb_sd_writer", 3072, NULL, 5,
            &s_usb_write_task, 1);
    }
    if (!cache_resources_ready || task_created != pdPASS) {
        ESP_LOGW(TAG, "USB MSC double write cache unavailable; using direct writes");
        s_usb_write_task = NULL;
    } else {
        ESP_LOGI(TAG,
                 "USB MSC double write-back cache ready: slots=%d slot_bytes=%u writer=CPU1",
                 STORAGE_USB_WRITE_SLOT_COUNT, STORAGE_USB_WRITE_CACHE_BYTES);
    }

    portENTER_CRITICAL(&s_usb_stats_mux);
    memset(&s_usb_read_stats, 0, sizeof(s_usb_read_stats));
    memset(&s_usb_write_stats, 0, sizeof(s_usb_write_stats));
    s_usb_stats_last_report_us = esp_timer_get_time();
    s_usb_stats_enabled = true;
    portEXIT_CRITICAL(&s_usb_stats_mux);

    ESP_LOGI(TAG, "USB MSC SD link: frequency=%d kHz width=%lu-bit host_limit=%d kHz",
             s_card->real_freq_khz, (unsigned long)(1U << s_card->log_bus_width),
             s_usb_host.max_freq_khz);
    ESP_LOGI(TAG, "SD card initialized for exclusive USB MSC access");
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Failed to initialize USB MSC SD medium: %s", esp_err_to_name(ret));
    storage_sdcard_release_usb_medium();
    return ret;
}

void storage_sdcard_release_usb_medium(void)
{
    if (!s_usb_medium_initialized) return;

    esp_err_t flush_ret = storage_sdcard_flush_usb_write_cache();
    if (flush_ret != ESP_OK) {
        ESP_LOGE(TAG, "USB MSC final cache flush failed: %s", esp_err_to_name(flush_ret));
    }

    portENTER_CRITICAL(&s_usb_stats_mux);
    s_usb_stats_enabled = false;
    portEXIT_CRITICAL(&s_usb_stats_mux);

    if (s_usb_host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        s_usb_host.deinit_p(s_usb_host.slot);
    } else if (s_usb_host.deinit) {
        (*s_usb_host.deinit)();
    }
    free(s_card);
    s_card = NULL;
    s_usb_medium_initialized = false;
    s_usb_do_transaction = NULL;
    if (s_usb_write_task != NULL && s_usb_write_queue != NULL) {
        const int stop = -1;
        xQueueSend(s_usb_write_queue, &stop, pdMS_TO_TICKS(100));
        xSemaphoreTake(s_usb_write_task_done, pdMS_TO_TICKS(1000));
        s_usb_write_task = NULL;
    }
    if (s_usb_write_gate != NULL) vSemaphoreDelete(s_usb_write_gate);
    if (s_usb_write_state_mutex != NULL) vSemaphoreDelete(s_usb_write_state_mutex);
    if (s_usb_write_free_slots != NULL) vSemaphoreDelete(s_usb_write_free_slots);
    if (s_usb_write_task_done != NULL) vSemaphoreDelete(s_usb_write_task_done);
    if (s_usb_write_queue != NULL) vQueueDelete(s_usb_write_queue);
    if (s_usb_write_events != NULL) vEventGroupDelete(s_usb_write_events);
    s_usb_write_gate = NULL;
    s_usb_write_state_mutex = NULL;
    s_usb_write_free_slots = NULL;
    s_usb_write_task_done = NULL;
    s_usb_write_queue = NULL;
    s_usb_write_events = NULL;
    for (int i = 0; i < STORAGE_USB_WRITE_SLOT_COUNT; ++i) {
        free(s_usb_write_slots[i].data);
        s_usb_write_slots[i].data = NULL;
        s_usb_write_slots[i].bytes = 0;
    }
    s_usb_write_active_slot = -1;
    s_usb_write_fake_next_status = false;
    storage_sdcard_deinit_power();
    ESP_LOGI(TAG, "USB MSC SD medium released");
}

void storage_sdcard_log_usb_transfer_stats(void)
{
    const int64_t now_us = esp_timer_get_time();
    storage_sdcard_io_stats_t reads = {0};
    storage_sdcard_io_stats_t writes = {0};
    int64_t window_us = 0;

    if (s_usb_write_gate != NULL && xSemaphoreTake(s_usb_write_gate, 0) == pdTRUE) {
        bool flush_idle = false;
        xSemaphoreTake(s_usb_write_state_mutex, portMAX_DELAY);
        if (s_usb_write_active_slot >= 0) {
            storage_sdcard_write_slot_t *active =
                &s_usb_write_slots[s_usb_write_active_slot];
            flush_idle = active->bytes != 0 &&
                now_us - s_usb_write_cache_last_append_us >=
                    STORAGE_USB_WRITE_IDLE_FLUSH_US;
        }
        xSemaphoreGive(s_usb_write_state_mutex);
        if (flush_idle) storage_sdcard_flush_under_gate();
        xSemaphoreGive(s_usb_write_gate);
    }

    portENTER_CRITICAL(&s_usb_stats_mux);
    if (s_usb_stats_enabled && s_usb_stats_last_report_us != 0 &&
        now_us - s_usb_stats_last_report_us >= STORAGE_USB_STATS_INTERVAL_US) {
        window_us = now_us - s_usb_stats_last_report_us;
        reads = s_usb_read_stats;
        writes = s_usb_write_stats;
        memset(&s_usb_read_stats, 0, sizeof(s_usb_read_stats));
        memset(&s_usb_write_stats, 0, sizeof(s_usb_write_stats));
        s_usb_stats_last_report_us = now_us;
    }
    portEXIT_CRITICAL(&s_usb_stats_mux);

    if (window_us == 0) return;
    storage_sdcard_log_one_io("read", &reads, window_us);
    storage_sdcard_log_one_io("write", &writes, window_us);
}

void storage_sdcard_unmount(void)
{
    if (s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(STORAGE_SDCARD_MOUNT_POINT, s_card);
        s_card = NULL;
        ESP_LOGI(TAG, "SD card unmounted");
    }

    storage_sdcard_deinit_power();
}

sdmmc_card_t *storage_sdcard_get_card(void)
{
    return s_card;
}

const char *storage_sdcard_get_mount_point(void)
{
    return STORAGE_SDCARD_MOUNT_POINT;
}
