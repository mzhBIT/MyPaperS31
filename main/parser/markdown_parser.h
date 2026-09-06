#pragma once

#include "parser/epub_html.h"

#include <string>
#include <vector>

struct MarkdownTocItem {
    std::string title;
    int depth = 0;
};

// Parses UTF-8 Markdown into the same neutral blocks used by the reflow
// reader. Image hrefs are resolved against source_path.
bool markdown_parse_blocks(const std::string &source,
                           const std::string &source_path,
                           std::vector<EpubContentBlock> *blocks,
                           std::vector<MarkdownTocItem> *toc = nullptr);
