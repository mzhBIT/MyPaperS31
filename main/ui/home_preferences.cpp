#include "ui/home_preferences.h"

#include <nvs.h>

namespace {

constexpr char NVS_NAMESPACE[] = "home_ui";
constexpr char NVS_ICON_FRAMES[] = "icon_frames";

}  // namespace

bool home_icon_frames_enabled()
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return true;
    uint8_t enabled = 1;
    (void)nvs_get_u8(handle, NVS_ICON_FRAMES, &enabled);
    nvs_close(handle);
    return enabled != 0;
}

esp_err_t home_set_icon_frames_enabled(bool enabled)
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, NVS_ICON_FRAMES, enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}
