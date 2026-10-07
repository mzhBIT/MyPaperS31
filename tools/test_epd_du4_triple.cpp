#include "lgfx/v1/platforms/esp32/EpdDu4Triple.h"
#include <cassert>
#include <cstdio>
#include <random>

static unsigned bucket(unsigned gray) {
    return gray <= 2 ? 0 : gray <= 7 ? 5 : gray <= 12 ? 10 : 15;
}
static uint32_t reference(const uint8_t* from, const uint8_t* to,
                          const uint8_t* transition) {
    static constexpr unsigned shifts[8] = {4, 0, 12, 8, 20, 16, 28, 24};
    uint32_t result = 0;
    for (unsigned pair = 0; pair < 8; ++pair) {
        const unsigned high = transition[(from[pair] & 0xf0) | bucket(to[pair] >> 4)];
        const unsigned low = transition[((from[pair] & 15) << 4) | bucket(to[pair] & 15)];
        result |= uint32_t((high << 2) | low) << shifts[pair];
    }
    return result;
}
int main() {
    std::mt19937 random(723);
    uint8_t tables[3][4096], transitions[3][256], codes[256];
    static constexpr unsigned targets[4] = {0, 5, 10, 15};
    for (unsigned value = 0; value < 256; ++value) {
        auto code = [](unsigned gray) { return gray <= 2 ? 0 : gray <= 7 ? 1 : gray <= 12 ? 2 : 3; };
        codes[value] = uint8_t((code(value >> 4) << 2) | code(value & 15));
    }
    size_t checked = 0;
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        for (unsigned frame = 0; frame < 3; ++frame) {
            for (unsigned transition = 0; transition < 256; ++transition)
                transitions[frame][transition] = scenario < 4 ? uint8_t((scenario + frame) & 3)
                                                           : uint8_t(random() & 3);
            for (unsigned from = 0; from < 256; ++from)
                for (unsigned code = 0; code < 16; ++code) {
                    const unsigned high = transitions[frame][(from & 0xf0) | targets[code >> 2]];
                    const unsigned low = transitions[frame][((from & 15) << 4) | targets[code & 3]];
                    tables[frame][(from << 4) | code] = uint8_t((high << 2) | low);
                }
        }
        // Every old/new byte pair, including unchanged pairs and all 16 grays.
        for (unsigned pair = 0; pair < 65536; ++pair) {
            uint8_t from[8], to[8];
            for (unsigned i = 0; i < 8; ++i) {
                from[i] = uint8_t((pair >> 8) + i * 17);
                to[i] = uint8_t(pair + i * 29);
            }
            uint32_t words[3];
            lgfx_epd_du4::group3(from, to, codes, tables[0], tables[1], tables[2],
                                 words[0], words[1], words[2]);
            for (unsigned frame = 0; frame < 3; ++frame)
                assert(words[frame] == reference(from, to, transitions[frame]));
            ++checked;
        }
    }
    std::printf("DU4 triple-frame kernel: %zu groups match independent scalar waveform reference\n", checked);
}
