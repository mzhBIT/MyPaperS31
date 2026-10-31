#pragma once
#include <cstddef>
#include <cstdint>

namespace emulator {
constexpr int PanelWidth = 1216, PanelHeight = 684;
constexpr int ScreenWidth = 684, ScreenHeight = 1216;
constexpr int GameWidth = 160, GameHeight = 144, Scale = 3;
constexpr int GameX = 102, GameY = 160;
// Same portrait transform as the S31 panel offset 3: px=y, py=683-x.
constexpr int VideoX = GameY, VideoY = PanelHeight - GameX - GameWidth * Scale;
constexpr int VideoWidth = GameHeight * Scale, VideoHeight = GameWidth * Scale;
constexpr size_t ImageBytes = PanelWidth * PanelHeight / 8;
constexpr size_t VideoBytes = VideoWidth * GameWidth / 8;
constexpr size_t StateBytes = VideoWidth * GameWidth / 2;
static_assert(VideoX % 8 == 0 && VideoWidth % 8 == 0 && VideoY >= 2);
static_assert(VideoX + VideoWidth <= PanelWidth && VideoY + VideoHeight <= PanelHeight);

// Paperboy MSG pair-state machine (Wenting Zhang, MIT; see vendor/NOTICE.md).
// Low two bits are the black/white targets; two 3-bit counters stop at four.
inline uint8_t drive_pair(uint8_t &state, uint8_t direction) {
    constexpr uint8_t reset[4] = {0xfc, 0xe0, 0x1c, 0x00};
    state = (state & reset[(state ^ direction) & 3]) | direction;
    const uint8_t output = ((state & 0x80)    ? 0
                            : (direction & 2) ? 4
                                              : 8) |
                           ((state & 0x10)    ? 0
                            : (direction & 1) ? 1
                                              : 2);
    state += ((~state) >> 2) & 0x24;
    return output;
}
inline uint8_t image_drive(uint8_t four_white_bits) {
    uint8_t out = 0;
    for (unsigned p = 0; p < 4; ++p)
        if (!(four_white_bits & (8 >> p)))
            out |= uint8_t(1 << ((3 - p) * 2));
    return out;
}
// GB LCD is four 2-bit pixels/byte, least significant pixel first.
// Four shades become 0/1/2/3 black subpixels, exactly the Paperboy scheme.
inline void encode_lcd(const uint8_t *lcd, uint8_t *bitmap) {
    for (int row = 0; row < GameWidth; ++row) {
        uint8_t *out = bitmap + row * (VideoWidth / 8);
        for (int b = 0; b < VideoWidth / 8; ++b)
            out[b] = 0;
        const int gx = GameWidth - 1 - row;
        for (int gy = 0; gy < GameHeight; ++gy) {
            const unsigned shade = (lcd[gy * 40 + gx / 4] >> ((gx & 3) * 2)) & 3;
            for (unsigned sub = 0; sub < shade; ++sub) {
                const int px = gy * 3 + sub;
                out[px / 8] |= uint8_t(0x80 >> (px & 7));
            }
        }
    }
}
} // namespace emulator
