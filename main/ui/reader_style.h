#pragma once

#include "ui/ReaderProfile.h"

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
    int full_refresh_pages = 30;    // Force one quality refresh after this many non-quality page refreshes.
    int underline_type = 0;         // 0=None, 1=Solid, 2=Dashed.
    int hide_header = 0;            // Hide reader top bar and let content use the extra vertical space.
    int letter_spacing = 0;         // Extra pixels between adjacent characters.
};

void reader_style_init();
ReaderStyle reader_style_get(ReaderProfile profile = ReaderProfile::Shared);
void reader_style_set(const ReaderStyle &style, ReaderProfile profile = ReaderProfile::Shared);
bool reader_style_save(ReaderProfile profile = ReaderProfile::Shared);
ReaderStyle reader_style_clamp(ReaderStyle style);
const char *reader_style_refresh_mode_name(int mode);
int reader_style_next_refresh_mode(int mode, int delta);
const char *reader_style_underline_name(int type);
int reader_style_next_underline_type(int type, int delta);
// Separate NVS key preserves the existing style_v7 blob and RSS profile.
constexpr int READER_AUTO_PAGE_MAX_SECONDS = 300;
constexpr int reader_auto_page_clamp_seconds(int seconds)
{
    return (seconds < 0 ? 0 : seconds > READER_AUTO_PAGE_MAX_SECONDS
        ? READER_AUTO_PAGE_MAX_SECONDS : seconds) / 5 * 5;
}
int reader_auto_page_seconds(ReaderProfile profile = ReaderProfile::Shared);
bool reader_auto_page_set_seconds(int seconds, ReaderProfile profile = ReaderProfile::Shared);
