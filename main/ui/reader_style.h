#pragma once

struct ReaderStyle {
    int font_size = 36;
    int line_spacing = 8;
    int margin_left = 20;
    int margin_right = 20;
    int margin_top = 5;
    int margin_bottom = 10;
    int refresh_mode = 3;
    int show_images = 0;
    int first_line_indent = 2;      // Full-width character cells.
    int paragraph_spacing = 0;      // Tenths of a line: 0..20 means 0.0..2.0 lines.
};

void reader_style_init();
ReaderStyle reader_style_get();
void reader_style_set(const ReaderStyle &style);
bool reader_style_save();
ReaderStyle reader_style_clamp(ReaderStyle style);
const char *reader_style_refresh_mode_name(int mode);
