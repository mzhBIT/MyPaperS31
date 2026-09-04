#include "ui/SystemFont.h"

#include <algorithm>

namespace {

bool ensure_ready()
{
    return true;
}

size_t utf8_advance(const std::string &text, size_t pos)
{
    if (pos >= text.size()) return pos;
    unsigned char c = (unsigned char)text[pos];
    size_t step = 1;
    if ((c & 0xE0) == 0xC0) step = 2;
    else if ((c & 0xF0) == 0xE0) step = 3;
    else if ((c & 0xF8) == 0xF0) step = 4;
    return std::min(text.size(), pos + step);
}

} // namespace

namespace system_font {

bool begin()
{
    return ensure_ready();
}

int line_height()
{
    M5.Display.setTextSize(1);
    return M5.Display.fontHeight(&fonts::lv_font_SourceHanSansNormalCN_36);
}

int measure(const std::string &text)
{
    M5.Display.setTextSize(1);
    return M5.Display.textWidth(text.c_str(), &fonts::lv_font_SourceHanSansNormalCN_36);
}

std::string ellipsize(const std::string &text, int max_width)
{
    if (max_width <= 0 || measure(text) <= max_width) return text;

    const std::string suffix = "...";
    int suffix_w = measure(suffix);
    if (suffix_w >= max_width) return suffix;

    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t next = utf8_advance(text, pos);
        std::string candidate = out + text.substr(pos, next - pos);
        if (measure(candidate) + suffix_w > max_width) break;
        out = candidate;
        pos = next;
    }
    return out + suffix;
}

void draw_left(lgfx::LGFXBase &gfx, const std::string &text, int x, int y, uint16_t color)
{
    if (!ensure_ready()) return;
    gfx.setTextSize(1);
    gfx.setTextDatum(textdatum_t::top_left);
    gfx.setTextColor(color, color);
    gfx.drawString(text.c_str(), x, y, &fonts::lv_font_SourceHanSansNormalCN_36);
}

void draw_center(lgfx::LGFXBase &gfx, const std::string &text, int cx, int y, uint16_t color)
{
    draw_left(gfx, text, cx - measure(text) / 2, y, color);
}

void draw_right(lgfx::LGFXBase &gfx, const std::string &text, int right_x, int y, uint16_t color)
{
    draw_left(gfx, text, right_x - measure(text), y, color);
}

} // namespace system_font
