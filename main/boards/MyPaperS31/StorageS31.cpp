#include "Pins.h"
#include "storage/storage_sdcard.h"
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <esp_log.h>

namespace { sdmmc_card_t* card = nullptr; }
extern "C" esp_err_t storage_sdcard_mount() {
    if (card) return ESP_OK;
    using namespace mypaper::board::s31;
    for (int pin : {sd_clk, sd_cmd, sd_data[0], sd_data[1], sd_data[2], sd_data[3]})
        gpio_hold_dis(static_cast<gpio_num_t>(pin));
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // Conservative bring-up clock; no UHS / voltage switching.
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = static_cast<gpio_num_t>(sd_clk);
    slot.cmd = static_cast<gpio_num_t>(sd_cmd);
    slot.d0 = static_cast<gpio_num_t>(sd_data[0]);
    slot.d1 = static_cast<gpio_num_t>(sd_data[1]);
    slot.d2 = static_cast<gpio_num_t>(sd_data[2]);
    slot.d3 = static_cast<gpio_num_t>(sd_data[3]);
    slot.width = 4;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    cfg.format_if_mount_failed = false;
    // Same budget as P4: 12 UI banks can retain 24 SD font faces;
    // leave eight handles for books, ZIPs, thumbnails and other application I/O.
    cfg.max_files = 32;
    cfg.allocation_unit_size = 16 * 1024;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(STORAGE_SDCARD_MOUNT_POINT,
                                                &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        card = nullptr;
        ESP_LOGE("S31_SD", "mount failed (never format): %s", esp_err_to_name(err));
    } else {
        sdmmc_card_print_info(stdout, card);
        ESP_LOGI("S31_SD", "mounted /sdcard 4-bit CLK=%d CMD=%d D0..3=%d,%d,%d,%d",
                 sd_clk, sd_cmd, sd_data[0], sd_data[1], sd_data[2], sd_data[3]);
    }
    return err;
}
extern "C" void storage_sdcard_unmount() {
    if (card && esp_vfs_fat_sdcard_unmount(STORAGE_SDCARD_MOUNT_POINT, card) == ESP_OK)
        card = nullptr;
}
extern "C" sdmmc_card_t* storage_sdcard_get_card() { return card; }
extern "C" const char* storage_sdcard_get_mount_point() { return STORAGE_SDCARD_MOUNT_POINT; }
extern "C" esp_err_t storage_sdcard_init_usb_medium() { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void storage_sdcard_release_usb_medium() {}
extern "C" void storage_sdcard_log_usb_transfer_stats() {}
extern "C" esp_err_t storage_sdcard_flush_usb_write_cache() { return ESP_ERR_NOT_SUPPORTED; }
