#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace bookshelf_font_fit {
constexpr int estimate(int px, int width, int measured) {
    return std::max(8, std::min(px-1, int(int64_t(px)*width/measured)-1));
}
// Survives activity destruction. Key by font generation, full translated text,
// preferred size and available width; no SD font handles are held here.
class Cache {
    struct Entry {
        std::string text;
        uint32_t generation = 0;
        int preferred = 0, width = 0, result = 0;
    };
    std::array<Entry,32> entries{};
    unsigned next = 0;
public:
    template<class Measure>
    int fit(const std::string &text, int preferred, int width,
            uint32_t generation, Measure measure) {
        for (const auto &e : entries)
            if (e.result && e.generation==generation && e.preferred==preferred
             && e.width==width && e.text==text) return e.result;
        int px=preferred;
        // At most three distinct-size measurements, rather than N one-pixel
        // decrements. A 1px safety margin absorbs hinting/rounding differences.
        for (int attempt=0; attempt<3; ++attempt) {
            const int measured=measure(text,px);
            if (measured<=width || px<=8) break;
            px=attempt==2 ? 8 : estimate(px,width,measured);
        }
        entries[next]={text,generation,preferred,width,px};
        next=(next+1)%entries.size();
        return px;
    }
};
}
