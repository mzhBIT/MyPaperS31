#pragma once
#include "ui/reader_style.h"
#include <M5Unified.h>
#include "ui/ReaderSwipe.h"
#include "ui/ReaderTap.h"

inline void reader_request_page_animation(int direction, epd_mode_t mode,
                                         ReaderProfile profile = ReaderProfile::Shared)
{
    auto *panel = M5.Display.getPanel();
    if (!panel) return;
    panel->setPageTurnAnimation(direction != 0 && mode == epd_mode_t::epd_text &&
        reader_page_animation_enabled(profile) ? direction : 0,
        reader_page_animation_mode(profile)==ReaderAnimationMode::Slow ? 2 : 1);
}
