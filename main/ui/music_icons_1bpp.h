#pragma once

#include "ui/icon_bitmap_1bpp.h"

#include <cstdint>

enum class MusicIconId : uint8_t {
    Previous,
    Play,
    Pause,
    Next,
    Shuffle,
    Sequence,
    RepeatOne,
    Snowflake,
    Playlist,
    VolumeDown,
    VolumeUp,
    Count,
};

namespace music_icons {

const IconBitmap1bpp *get(MusicIconId id);

}  // namespace music_icons
