#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace embedded_font {
constexpr const char *path = "builtin:system-ui";
extern const uint8_t start[] asm("_binary_system_ui_ttf_start");
extern const uint8_t end[] asm("_binary_system_ui_ttf_end");
inline size_t size() { return static_cast<size_t>(end - start); }
inline bool matches(const char *value) {
    // Old saved reader configurations remain readable after the LV bitmap is removed.
    return value && (!std::strcmp(value, path) ||
                     !std::strcmp(value, "builtin:source_han_sans_cn_36"));
}
}
