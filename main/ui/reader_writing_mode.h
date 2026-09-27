#pragma once

#include <cstdint>

namespace reader_writing_mode {

enum class Mode : uint8_t { Auto = 0, Horizontal = 1, VerticalRl = 2 };

Mode book_mode(const char *path);
bool set_book_mode(const char *path, Mode mode);

} // namespace reader_writing_mode
