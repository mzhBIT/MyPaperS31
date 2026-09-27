#pragma once

#include "renderer/TextLayout.h"
#include "renderer/InlineStyle.h"
#include "ui/script_conversion.h"

#include <cstdint>
#include <string>
#include <vector>

namespace document_layout {

static constexpr uint32_t ALGORITHM_VERSION = 6;

enum class BlockType {
    Paragraph,
    Image,
    HorizontalRule,
    PageBreak,
};

struct Block {
    BlockType type = BlockType::Paragraph;
    std::string text;
    std::string source_text;
    std::vector<script_conversion::Point> source_points;
    std::string resource;
    uint64_t content_position = 0;
    int image_width = 0;
    int image_height = 0;
    int blank_before = 0;
    int blank_after = 1;
    bool heading = false;
    int heading_level = 0;
    bool page_break_before = false;
    bool allow_first_line_indent = false;
    bool paragraph_spacing_after = false;
    struct FootnoteRef {
        size_t byte_start = 0;
        size_t byte_end = 0;
        std::string target;
        std::string content;
    };
    std::vector<FootnoteRef> footnotes;
    std::vector<inline_style::Span> styles;
};

struct ImageInfo {
    int width = 0;
    int height = 0;
};

enum class PageItemType {
    Text,
    Space,
    Image,
    HorizontalRule,
};

struct PageItem {
    PageItemType type = PageItemType::Text;
    std::string text;
    std::string source_text;
    std::string resource;
    int width = 0;
    int height = 0;
    float scale = 1.0f;
    uint64_t content_position = 0;
    bool justify = false;
    int x_offset = 0;
    bool centered = false;
    struct FootnoteRef {
        size_t byte_start = 0;
        size_t byte_end = 0;
        std::string target;
        std::string content;
    };
    std::vector<FootnoteRef> footnotes;
    std::vector<inline_style::Span> styles;

    // Image layout diagnostics retained for format-specific logging.
    int source_width = 0;
    int source_height = 0;
    int declared_width = 0;
    int declared_height = 0;
    int remaining_height = 0;
    bool fitted_current_page = false;
};

using Page = std::vector<PageItem>;
using Pages = std::vector<Page>;

using LayoutParagraphFn = std::vector<text_layout::Line> (*)(
    void *context, const std::string &text, int max_width, int first_line_indent);
using ResolveImageFn = bool (*)(
    void *context, const std::string &resource, ImageInfo *info);

struct Options {
    int content_width = 0;
    int content_height = 0;
    int line_height = 0;
    int first_line_indent = 0;
    int paragraph_spacing_percent = 0;

    // A value of 1 disables orphan/widow protection and fills each page as
    // far as its available height permits.
    int orphan_lines = 1;
    int widow_lines = 1;
    int heading_following_lines = 2;
    int image_fit_remaining_percent = 50;
    bool allow_image_upscale = false;
    // Legacy MOBI markup may contain dimensions expressed in old device or
    // non-pixel units. Ignore a tiny declared box for a large page image.
    bool ignore_tiny_declared_image_size = false;
    const char *image_fallback_text = "[image]";

    LayoutParagraphFn layout_paragraph = nullptr;
    void *text_context = nullptr;
    ResolveImageFn resolve_image = nullptr;
    void *image_context = nullptr;
};

void fit_image_box(int source_width, int source_height,
                   int max_width, int max_height,
                   bool allow_upscale,
                   int *draw_width, int *draw_height, float *scale);

Pages paginate(const std::vector<Block> &blocks, const Options &options);

} // namespace document_layout
