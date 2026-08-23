#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace embedded_lyrics {

struct Line {
    uint32_t time_ms = 0;
    std::string text;
};

// Loads synchronized embedded lyrics. Unsynchronized text is deliberately
// ignored because the player cannot scroll it against playback time.
bool load(const char *path, std::vector<Line> *lines);

}  // namespace embedded_lyrics
