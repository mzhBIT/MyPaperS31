#pragma once

#include <M5Unified.h>
#include <string>
#include <vector>
#include <esp_err.h>

namespace system_font {

bool begin();
constexpr int default_size = 28;
constexpr int min_size = 24;
constexpr int max_size = 48;
int pixel_size();
std::string font_path();
std::vector<std::string> available_fonts();
esp_err_t set_font(const std::string &path);
esp_err_t set_size(int pixels);
uint32_t generation();
int line_height(int pixels = 0);
int measure(const std::string &text, int pixels = 0);
std::string ellipsize(const std::string &text, int max_width);

void draw_left(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, uint16_t color = TFT_BLACK);
void draw_center(lgfx::LGFXBase &gfx, const std::string &text, int cx, int y, uint16_t color = TFT_BLACK);
void draw_right(lgfx::LGFXBase &gfx, const std::string &text, int right_x, int y, uint16_t color = TFT_BLACK);
void draw_left_sized(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, int pixels,
                     uint16_t color = TFT_BLACK);
void draw_right_sized(lgfx::LGFXBase &gfx, const std::string &text, int right_x, int y, int pixels,
                      uint16_t color = TFT_BLACK);

} // namespace system_font
