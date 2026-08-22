/*
 * Compiles the LVGL font accessor functions with LV_FONT_FMT_TXT_LARGE=1.
 * Large fonts (bitmap_index > 20 bits) need 16-byte glyph descriptors.
 */
#define LV_FONT_FMT_TXT_LARGE 1

#include "font.h"
#include "font_fmt_txt.h"

#define lv_font_get_glyph_dsc_fmt_txt lv_font_get_glyph_dsc_fmt_txt_large
#define lv_font_get_bitmap_fmt_txt      lv_font_get_bitmap_fmt_txt_large

#include "font_fmt_txt.c"
