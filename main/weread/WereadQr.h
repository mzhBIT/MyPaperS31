#pragma once

#include <M5GFX.h>
#include <string>

namespace weread {

bool draw_qr(lgfx::LGFXBase &display, const std::string &text,
             int x, int y, int size);

}  // namespace weread
