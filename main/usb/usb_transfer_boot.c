#include "usb/usb_transfer_boot.h"

#include "nvs.h"

#define USB_TRANSFER_NVS_NAMESPACE "usb_transfer"
#define USB_TRANSFER_NVS_BOOT_KEY  "boot_msc"

bool usb_transfer_boot_requested(void)
{
    nvs_handle_t handle;
    if (nvs_open(USB_TRANSFER_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t enabled = 0;
    esp_err_t ret = nvs_get_u8(handle, USB_TRANSFER_NVS_BOOT_KEY, &enabled);
    nvs_close(handle);
    return ret == ESP_OK && enabled == 1;
}

esp_err_t usb_transfer_boot_set(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(USB_TRANSFER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    if (enabled) ret = nvs_set_u8(handle, USB_TRANSFER_NVS_BOOT_KEY, 1);
    else ret = nvs_erase_key(handle, USB_TRANSFER_NVS_BOOT_KEY);
    if (ret == ESP_OK || (!enabled && ret == ESP_ERR_NVS_NOT_FOUND)) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}
