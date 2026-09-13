#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_SDCARD_MOUNT_POINT "/sdcard"

esp_err_t storage_sdcard_mount(void);
void storage_sdcard_unmount(void);
/** Initialize the SDMMC medium without registering a VFS mount point. */
esp_err_t storage_sdcard_init_usb_medium(void);
/** Release a medium initialized by storage_sdcard_init_usb_medium(). */
void storage_sdcard_release_usb_medium(void);
/** Emit a low-frequency aggregate of raw USB-MSC SD traffic, if any. */
void storage_sdcard_log_usb_transfer_stats(void);
/** Commit any pending USB-MSC write-back data to the SD card. */
esp_err_t storage_sdcard_flush_usb_write_cache(void);
sdmmc_card_t *storage_sdcard_get_card(void);
const char *storage_sdcard_get_mount_point(void);

#ifdef __cplusplus
}
#endif
