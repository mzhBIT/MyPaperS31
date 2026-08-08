#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_SDCARD_MOUNT_POINT "/sdcard"

esp_err_t storage_sdcard_mount(void);
void storage_sdcard_unmount(void);
sdmmc_card_t *storage_sdcard_get_card(void);
const char *storage_sdcard_get_mount_point(void);

#ifdef __cplusplus
}
#endif
