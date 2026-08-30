#pragma once

#include <cstddef>
#include <string>

namespace ui::utf8 {

bool valid(const std::string &text);
size_t length(const std::string &text);
bool erase_last(std::string *text);
std::string truncate_bytes(const std::string &text, size_t max_bytes);
bool append(std::string *text, const std::string &suffix, size_t max_bytes);

}  // namespace ui::utf8
