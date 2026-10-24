#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace weread::position {
// Map a unique original-text excerpt to Web coordinates: explicit data-wr-co
// when present, otherwise the pristine HTML/TXT UTF-16 source position.
// HTML markup and entity source lengths count, not reflow/visible text bytes.
// Ambiguous/missing excerpts are deliberately not uploadable.
bool locate(std::string_view source, std::string_view excerpt, bool html,
            uint64_t *offset, const char **reason = nullptr);
}
