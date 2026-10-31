#include "boot/firmware_switch.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <cstring>

namespace firmware_switch {
namespace {
esp_err_t find(bool emulator, const esp_partition_t **result) {
#if CONFIG_IDF_TARGET_ESP32S31
    constexpr const char *system_name = "MyPaperS31";
    constexpr const char *emulator_name = "MyPaperEmulatorS31";
#else
    (void)emulator; (void)result;
    return ESP_ERR_NOT_SUPPORTED;
#endif
#if CONFIG_IDF_TARGET_ESP32S31
    const auto *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        emulator ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_FACTORY,
        emulator ? "emulator" : "factory");
    if (!part) return ESP_ERR_NOT_FOUND;
    if (part->address != (emulator ? 0x810000u : 0x10000u) ||
        part->size != (emulator ? 0x400000u : 0x800000u)) return ESP_ERR_INVALID_SIZE;
    const auto *ota = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    if (!ota || ota->address != 0xC10000u || ota->size != 0x2000u) return ESP_ERR_NOT_FOUND;
    esp_app_desc_t desc{};
    const auto error = esp_ota_get_partition_description(part, &desc);
    if (error != ESP_OK) return error;
    const char *expected = emulator ? emulator_name : system_name;
    if (std::strncmp(desc.project_name, expected, sizeof(desc.project_name)) != 0)
        return ESP_ERR_INVALID_VERSION;
    if (emulator) {
        const esp_partition_t *system = nullptr;
        const auto system_error = find(false, &system);
        if (system_error != ESP_OK) return system_error;
    }
    *result = part;
    return ESP_OK;
#endif
}
esp_err_t select(bool emulator) {
    const esp_partition_t *part = nullptr;
    const auto error = find(emulator, &part);
    return error == ESP_OK ? esp_ota_set_boot_partition(part) : error;
}
}
esp_err_t check_emulator() { const esp_partition_t *part=nullptr; return find(true,&part); }
esp_err_t select_emulator() { return select(true); }
esp_err_t select_system() { return select(false); }
}
