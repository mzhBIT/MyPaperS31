#pragma once

#include <M5Unified.h>
#include "renderer/TextLayout.h"
#include "renderer/InlineStyle.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ReaderTextStyle {
    const char *font_path = "builtin:source_han_sans_cn_36";
    int font_size = 36;
    int line_spacing = 8;
    int letter_spacing = 0;
    int glyph_cache_limit = 2048;
    int advance_cache_limit = 768;
    int glyph_pool_bytes = 768 * 1024;
};

struct TextRenderStats {
    size_t glyph_count = 0;
    size_t glyph_pool_used = 0;
    size_t glyph_pool_capacity = 0;
    size_t advance_count = 0;
    uint32_t missing_glyphs = 0;
};

class TextRenderer {
public:
    bool begin(const ReaderTextStyle &style = ReaderTextStyle());
    bool usingFreeType() const { return _ft_ready; }
    bool usingBinFont() const { return _bin_ready; }
    bool usingLvFont() const { return _lv_ready; }

    int lineHeight() const;
    int baseline() const;
    int measureCodepoint(uint32_t codepoint);
    int measureCodepointForLayout(uint32_t codepoint);
    int measureKerningForLayout(uint32_t left, uint32_t right);
    int measureFullwidthCellForLayout();
    int measureUtf8(const std::string &text);
    std::vector<text_layout::Line> layoutUtf8Paragraph(const std::string &text, int max_width,
                                                        int first_line_indent = 0);
    std::vector<text_layout::Line> layoutUtf8VerticalParagraph(const std::string &text,
                                                               int max_height,
                                                               int first_column_indent = 0);
    std::vector<text_layout::Line> layoutUtf8VerticalParagraphStyled(
        const std::string &text, const std::vector<inline_style::Span> &styles,
        int max_height, int first_column_indent = 0);
    int verticalCellAdvance() const;
    int verticalRubyLaneWidth() const;
    int measureUtf8Vertical(const std::string &text) const;
    int measureUtf8VerticalStyled(const std::string &text,
                                  const std::vector<inline_style::Span> &styles,
                                  size_t byte_end = std::string::npos) const;
    void drawStyledUtf8VerticalColumn(lgfx::LGFXBase &gfx, const std::string &text,
                                      const std::vector<inline_style::Span> &styles,
                                      int x, int y, int column_width, uint16_t color);
    bool drawUtf8VerticalRuby(lgfx::LGFXBase &gfx, const std::string &text,
                              int x, int y, int base_height, int lane_width,
                              uint16_t color);
    std::vector<std::string> wrapUtf8Line(const std::string &line, int max_width);
    bool preparePage(const std::string &text);
    void drawUtf8Line(lgfx::LGFXBase &gfx, const std::string &text, int x, int y,
                      uint16_t color, int justify_width = 0);
    void drawStyledUtf8Line(lgfx::LGFXBase &gfx, const std::string &text,
                            const std::vector<inline_style::Span> &styles,
                            int x, int y, uint16_t color, int justify_width = 0);
    void clearGlyphCache();
    TextRenderStats stats() const;
    std::string layoutSignature() const;

private:
    struct Glyph {
        uint32_t codepoint = 0;
        int advance = 0;
        int bitmap_left = 0;
        int bitmap_top = 0;
        int width = 0;
        int height = 0;
        int pitch = 0;
        size_t data_offset = 0;
        size_t data_size = 0;
        uint32_t last_used = 0;
    };
    struct Advance {
        uint32_t codepoint = 0;
        int advance = 0;
        uint32_t last_used = 0;
    };
    struct LookupSlot {
        uint32_t codepoint = 0;
        uint16_t index = 0;
        uint8_t state = 0; // 0=empty, 1=used, 2=deleted
    };
    struct PerfCounters {
        uint32_t glyph_loads = 0;
        uint32_t advance_loads = 0;
        uint32_t bin_glyph_loads = 0;
        uint32_t bin_entry_reads = 0;
        uint32_t bin_bitmap_reads = 0;
        unsigned long bin_bitmap_bytes = 0;
        int64_t advance_load_us = 0;
        int64_t glyph_load_char_us = 0;
        int64_t glyph_bbox_us = 0;
        int64_t glyph_clear_us = 0;
        int64_t glyph_raster_us = 0;
        int64_t glyph_total_us = 0;
        int64_t bin_entry_us = 0;
        int64_t bin_bitmap_read_us = 0;
        int64_t bin_decode_us = 0;
        int64_t bin_total_us = 0;
        int64_t slowest_glyph_us = 0;
        uint32_t slowest_glyph_cp = 0;
    };
    struct BinFontEntry {
        uint32_t codepoint = 0;
        uint16_t advance = 0;
        uint8_t width = 0;
        uint8_t height = 0;
        int8_t x_offset = 0;
        int8_t y_offset = 0;
        uint32_t bitmap_offset = 0;
        uint32_t bitmap_size = 0;
    };
    enum BinFontEncoding {
        BIN_FONT_ENCODING_UNKNOWN = 0,
        BIN_FONT_ENCODING_EDC_BITSTREAM,
        BIN_FONT_ENCODING_RAW_4BPP,
    };

    void initLookupTables();
    int findLookup(const std::vector<LookupSlot> &table, uint32_t codepoint) const;
    void insertLookup(std::vector<LookupSlot> &table, uint32_t codepoint, uint16_t index);
    void eraseLookup(std::vector<LookupSlot> &table, uint32_t codepoint);
    bool loadAdvance(uint32_t codepoint, Advance &out);
    const Advance *getAdvance(uint32_t codepoint);
    bool loadGlyph(uint32_t codepoint, Glyph &out);
    const Glyph *getGlyph(uint32_t codepoint);
    size_t nextUtf8(const std::string &text, size_t pos, uint32_t &codepoint) const;
    std::string codepointToUtf8(uint32_t codepoint) const;
    void drawGlyph(lgfx::LGFXBase &gfx, const Glyph &glyph, int x, int baseline_y,
                   uint16_t color);
    void drawGlyphScaled(lgfx::LGFXBase &gfx, const Glyph &glyph,
                         int x, int baseline_y, int scale_percent, uint16_t color);
    void drawGlyphRuns(lgfx::LGFXBase &gfx, const Glyph &glyph, int x, int baseline_y, uint16_t color);
    void drawGlyphStyled(lgfx::LGFXBase &gfx, const Glyph &glyph, int x, int baseline_y,
                         uint16_t color, uint8_t style_flags);
    void preloadCommonGlyphs();
    void resetFont();
    bool compactGlyphCache(size_t needed_bytes);
    bool beginBinFont();
    bool beginLvFont();
    bool readBinDataAt(unsigned long offset, void *buffer, size_t size) const;
    bool loadBinFontEntry(uint32_t codepoint, BinFontEntry *out) const;
    bool loadBinGlyph(uint32_t codepoint, Glyph &out);
    bool readBinFontEntryAt(uint32_t index, BinFontEntry *out) const;
    void decodeBinFontEntry(const uint8_t *raw, BinFontEntry *out) const;
    void detectBinFontEncoding();
    void resetBinFont();

    ReaderTextStyle _style;
    std::string _active_font_path;
    bool _ft_ready = false;
    bool _bin_ready = false;
    bool _lv_ready = false;
    bool _ft_initialized = false;
    int _line_height = 44;
    int _baseline = 34;
    const lgfx::IFont *_lv_font = nullptr;
    void *_library = nullptr;
    void *_face = nullptr;
    void *_ft_memory = nullptr;
    void *_font_stream = nullptr;
    void *_font_stream_ctx = nullptr;
    int _bin_fd = -1;
    const uint8_t *_bin_data = nullptr;
    size_t _bin_data_size = 0;
    uint32_t _bin_char_count = 0;
    uint8_t _bin_font_height = 0;
    BinFontEncoding _bin_encoding = BIN_FONT_ENCODING_UNKNOWN;
    unsigned long _bin_file_size = 0;
    uint8_t *_bin_entry_table = nullptr;
    size_t _bin_entry_table_bytes = 0;
    std::vector<BinFontEntry> _bin_entry_cache;
    std::vector<Advance> _advance_cache;
    std::vector<Glyph> _glyph_cache;
    std::vector<uint32_t> _missing_codepoints;
    std::vector<uint32_t> _page_missing_codepoints;
    // Glyphs required by the last page passed to preparePage().
    std::vector<uint32_t> _prepared_page_codepoints;
    std::vector<LookupSlot> _advance_lookup;
    std::vector<LookupSlot> _glyph_lookup;
    uint8_t *_glyph_pool = nullptr;
    size_t _glyph_pool_capacity = 0;
    size_t _glyph_pool_used = 0;
    bool _glyph_pool_full = false;
    uint32_t _lru_tick = 1;
    PerfCounters *_perf = nullptr;
};

TextRenderer &reader_text_renderer();
