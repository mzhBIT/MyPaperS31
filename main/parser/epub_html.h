#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "renderer/InlineStyle.h"

struct EpubFootnoteRef {
    enum class Kind : uint8_t { LinkedFootnote, RubyText, InlineSup, WereadComment };
    size_t byte_start = 0;
    size_t byte_end = 0;
    std::string target;
    std::string content;
    Kind kind = Kind::LinkedFootnote;
};

struct EpubSourcePoint {
    size_t display = 0;
    size_t source = 0;
};

enum class EpubContentBlockType {
    Text,
    Image,
    HorizontalRule,
    PageBreak,
};

struct EpubTextBlock {
    std::string text;
    int blank_before = 0;
    int blank_after = 1;
    bool heading = false;
    int heading_level = 0;
    bool page_break_before = false;
};

struct EpubContentBlock {
    EpubContentBlockType type = EpubContentBlockType::Text;
    std::string text;
    std::string href;
    int image_width = 0;
    int image_height = 0;
    int blank_before = 0;
    int blank_after = 1;
    bool heading = false;
    int heading_level = 0;
    bool page_break_before = false;
    std::vector<EpubFootnoteRef> footnotes;
    size_t source_text_bytes = 0;
    std::vector<EpubSourcePoint> source_points;
    std::vector<inline_style::Span> styles;
};

bool epub_parse_xhtml_text_blocks(const std::string &xhtml, std::vector<EpubTextBlock> *blocks);
bool epub_parse_xhtml_blocks(const std::string &xhtml, const std::string &item_href,
                             std::vector<EpubContentBlock> *blocks,
                             bool inline_sup_annotations = false,
                             bool preserve_inline_styles = false);
bool epub_resolve_footnote(const std::string &xhtml, const std::string &item_href,
                           const std::string &target, std::string *content);
