#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct EpubFootnoteRef {
    size_t byte_start = 0;
    size_t byte_end = 0;
    std::string target;
    std::string content;
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
};

bool epub_parse_xhtml_text_blocks(const std::string &xhtml, std::vector<EpubTextBlock> *blocks);
bool epub_parse_xhtml_blocks(const std::string &xhtml, const std::string &item_href, std::vector<EpubContentBlock> *blocks);
bool epub_resolve_footnote(const std::string &xhtml, const std::string &item_href,
                           const std::string &target, std::string *content);
