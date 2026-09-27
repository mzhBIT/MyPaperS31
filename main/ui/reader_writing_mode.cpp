#include "ui/reader_writing_mode.h"

#include <cstdio>
#include <nvs.h>

namespace reader_writing_mode {
namespace {

constexpr char NVS_NAMESPACE[] = "writing_mode";

void key_for_path(const char *path, char (&key)[16])
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(path); *p; ++p)
        hash = (hash ^ *p) * 16777619u;
    std::snprintf(key, sizeof(key), "b%08lx", static_cast<unsigned long>(hash));
}

} // namespace

Mode book_mode(const char *path)
{
    if (!path || !*path) return Mode::Auto;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return Mode::Auto;
    char key[16];
    key_for_path(path, key);
    uint8_t value = 0;
    nvs_get_u8(handle, key, &value);
    nvs_close(handle);
    return value <= 2 ? static_cast<Mode>(value) : Mode::Auto;
}

bool set_book_mode(const char *path, Mode mode)
{
    if (!path || !*path || static_cast<uint8_t>(mode) > 2) return false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    char key[16];
    key_for_path(path, key);
    const bool ok = nvs_set_u8(handle, key, static_cast<uint8_t>(mode)) == ESP_OK &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

} // namespace reader_writing_mode
