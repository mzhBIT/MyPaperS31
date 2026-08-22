#pragma once

#include <string>
#include <vector>

enum class EpubContentBlockType {
    Text,
    Image,
};

struct EpubTextBlock {
    std::string text;
    int blank_before = 0;
    int blank_after = 1;
    bool heading = false;
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
};

bool epub_parse_xhtml_text_blocks(const std::string &xhtml, std::vector<EpubTextBlock> *blocks);
bool epub_parse_xhtml_blocks(const std::string &xhtml, const std::string &item_href, std::vector<EpubContentBlock> *blocks);
