#include "storage_sdcard.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

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

static const char *TAG = "storage_sdcard";

static sdmmc_card_t *s_card;

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

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.io_voltage = 3.3f;

    esp_err_t ret = storage_sdcard_init_power(&host);
    if (ret != ESP_OK) {
        return ret;
    }

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = STORAGE_SDCARD_PIN_CLK;
    slot_config.cmd = STORAGE_SDCARD_PIN_CMD;
    slot_config.d0 = STORAGE_SDCARD_PIN_D0;
    slot_config.d1 = STORAGE_SDCARD_PIN_D1;
    slot_config.d2 = STORAGE_SDCARD_PIN_D2;
    slot_config.d3 = STORAGE_SDCARD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

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
