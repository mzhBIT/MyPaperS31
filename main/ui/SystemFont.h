#pragma once

#include <M5Unified.h>
#include <string>

namespace system_font {

bool begin();
int line_height();
int measure(const std::string &text);
std::string ellipsize(const std::string &text, int max_width);

void draw_left(M5GFX &gfx, const std::string &text, int x, int y, uint16_t color = TFT_BLACK);
void draw_center(M5GFX &gfx, const std::string &text, int cx, int y, uint16_t color = TFT_BLACK);
void draw_right(M5GFX &gfx, const std::string &text, int right_x, int y, uint16_t color = TFT_BLACK);

} // namespace system_font
