#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace script_conversion {

enum class Mode : uint8_t { Original = 0, Simplified = 1, Traditional = 2 };

struct Point {
    size_t display = 0;
    size_t source = 0;
};

struct Result {
    std::string text;
    std::vector<Point> points;
    size_t source_offset(size_t display_offset) const;
    size_t display_offset(size_t source_offset) const;
};

const char *mode_name(Mode mode);
Mode book_mode(const char *path);
bool set_book_mode(const char *path, Mode mode);
// Loads and validates the selected dictionary into PSRAM. The active file is
// cached; switching direction replaces it. Original always succeeds.
bool prepare(Mode mode);
uint32_t dictionary_signature();
Result convert(const std::string &source, Mode mode);

} // namespace script_conversion
