#pragma once

#include <cstdint>

#define HOME_ICON_1BPP_WIDTH 150u
#define HOME_ICON_1BPP_HEIGHT 150u
#define HOME_ICON_1BPP_STRIDE 19u
#define HOME_ICON_1BPP_DATA_SIZE (HOME_ICON_1BPP_STRIDE * HOME_ICON_1BPP_HEIGHT)

enum class HomeIconId {
    Clock,
    Transfer,
    Images,
    Files,
    Book,
    News,
    Music,
    Radio,
    Setting,
    DeepSeek,
    WeChatReading,
    Map,
    Messages,
    Phone,
    Stocks,
    Calculator,
    Emulator,
    Camera,
    Count,
};

const uint8_t *home_icon_1bpp_data(HomeIconId id);
