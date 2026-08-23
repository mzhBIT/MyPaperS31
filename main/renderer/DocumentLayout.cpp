#include "renderer/DocumentLayout.h"

#include <algorithm>
#include <utility>

namespace document_layout {
namespace {

struct Builder {
    explicit Builder(const Options &layout_options)
        : options(layout_options)
    {
    }

    const Options &options;
    Pages pages;
    Page page;
    int used_height = 0;

    void finish_page()
    {
        while (!page.empty() && page.back().type == PageItemType::Space) {
            used_height -= page.back().height;
            page.pop_back();
        }
        if (!page.empty()) pages.push_back(std::move(page));
        page.clear();
        used_height = 0;
    }

    void ensure_space(int height)
    {
        if (!page.empty() && used_height + height > options.content_height) {
            finish_page();
        }
    }

    void add_space(uint64_t position, int height)
    {
        // Suppress top-of-page and trailing blank lines.
        if (page.empty()) return;
        PageItem item;
        item.type = PageItemType::Space;
        item.height = std::max(0, height);
        if (item.height == 0) return;
        item.content_position = position;
        ensure_space(item.height);
        if (!page.empty()) {
            int added_height = item.height;
            page.push_back(std::move(item));
            used_height += added_height;
        }
    }

    void add_text(const text_layout::Line &line, uint64_t block_position, bool centered)
    {
        if (line.text.empty()) {
            if (line.hard_break) add_space(block_position + line.byte_start,
                                           options.line_height);
            return;
        }
        PageItem item;
        item.type = PageItemType::Text;
        item.text = line.text;
        item.height = options.line_height;
        item.content_position = block_position + line.byte_start;
        item.justify = line.justify && !centered;
        item.x_offset = line.x_offset;
        item.width = std::max(1, options.content_width - line.x_offset);
        item.centered = centered;
        ensure_space(item.height);
        page.push_back(std::move(item));
        used_height += options.line_height;
    }

    void add_fallback_text(uint64_t position)
    {
        if (!options.image_fallback_text || !options.image_fallback_text[0]) return;
        text_layout::Line line;
        line.text = options.image_fallback_text;
        add_text(line, position, false);
    }

    void add_rule(uint64_t position)
    {
        PageItem item;
        item.type = PageItemType::HorizontalRule;
        item.height = options.line_height;
        item.content_position = position;
        ensure_space(item.height);
        page.push_back(std::move(item));
        used_height += options.line_height;
    }

    void add_image(const Block &block)
    {
        ImageInfo source;
        if (!options.resolve_image ||
            !options.resolve_image(options.image_context, block.resource, &source) ||
            source.width <= 0 || source.height <= 0) {
            add_fallback_text(block.content_position);
            return;
        }

        int preferred_width = options.content_width;
        int preferred_height = options.content_height;
        if (block.image_width > 0) {
            preferred_width = std::min(preferred_width, block.image_width);
        }
        if (block.image_height > 0) {
            preferred_height = std::min(preferred_height, block.image_height);
        }

        PageItem item;
        item.type = PageItemType::Image;
        item.resource = block.resource;
        item.content_position = block.content_position;
        item.source_width = source.width;
        item.source_height = source.height;
        item.declared_width = block.image_width;
        item.declared_height = block.image_height;
        fit_image_box(source.width, source.height,
                      preferred_width, preferred_height,
                      options.allow_image_upscale,
                      &item.width, &item.height, &item.scale);

        item.remaining_height = std::max(0, options.content_height - used_height);
        const bool enough_remainder =
            item.remaining_height * 100 >
            options.content_height * options.image_fit_remaining_percent;
        if (!page.empty() && item.height > item.remaining_height && enough_remainder) {
            int remainder_limit = std::min(preferred_height, item.remaining_height);
            fit_image_box(source.width, source.height,
                          preferred_width, remainder_limit,
                          options.allow_image_upscale,
                          &item.width, &item.height, &item.scale);
            item.fitted_current_page = true;
        }

        ensure_space(item.height);
        page.push_back(std::move(item));
        used_height += page.back().height;
    }
};

} // namespace

void fit_image_box(int source_width, int source_height,
                   int max_width, int max_height,
                   bool allow_upscale,
                   int *draw_width, int *draw_height, float *scale)
{
    if (draw_width) *draw_width = std::max(0, max_width);
    if (draw_height) *draw_height = std::max(0, max_height);
    if (scale) *scale = 1.0f;
    if (source_width <= 0 || source_height <= 0 || max_width <= 0 || max_height <= 0) return;

    float fitted_scale = allow_upscale
                       ? (float)max_width / (float)source_width
                       : 1.0f;
    if ((int)(source_width * fitted_scale) > max_width) {
        fitted_scale = (float)max_width / (float)source_width;
    }
    if ((int)(source_height * fitted_scale) > max_height) {
        fitted_scale = (float)max_height / (float)source_height;
    }
    if (fitted_scale <= 0.0f) fitted_scale = 1.0f;

    if (scale) *scale = fitted_scale;
    if (draw_width) *draw_width = std::max(1, (int)(source_width * fitted_scale));
    if (draw_height) *draw_height = std::max(1, (int)(source_height * fitted_scale));
}

Pages paginate(const std::vector<Block> &blocks, const Options &options)
{
    Builder builder(options);
    if (options.content_width <= 0 || options.content_height <= 0 ||
        options.line_height <= 0 || !options.layout_paragraph) {
        builder.pages.push_back(Page());
        return builder.pages;
    }

    int pending_blank_lines = 0;
    int pending_paragraph_spacing = 0;
    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        const Block &block = blocks[block_index];
        int gap_lines = std::max(pending_blank_lines, block.blank_before);
        int gap_height = std::max(gap_lines * options.line_height,
                                  pending_paragraph_spacing);

        if (block.page_break_before && !builder.page.empty()) builder.finish_page();
        if (block.type == BlockType::PageBreak) {
            builder.finish_page();
            pending_blank_lines = block.blank_after;
            pending_paragraph_spacing = 0;
            continue;
        } else if (block.type == BlockType::HorizontalRule) {
            builder.add_space(block.content_position, gap_height);
            builder.add_rule(block.content_position);
        } else if (block.type == BlockType::Image) {
            builder.add_space(block.content_position, gap_height);
            builder.add_image(block);
        } else {
            int indent = block.allow_first_line_indent
                       ? std::max(0, options.first_line_indent)
                       : 0;
            std::vector<text_layout::Line> wrapped = options.layout_paragraph(
                options.text_context, block.text, options.content_width, indent);

            int protected_lines = std::min<int>(
                std::max(1, options.orphan_lines), (int)wrapped.size());
            if (block.heading) {
                int following_gap = block.blank_after;
                bool followed_by_text = false;
                if (block_index + 1 < blocks.size()) {
                    following_gap = std::max(following_gap,
                                             blocks[block_index + 1].blank_before);
                    followed_by_text = blocks[block_index + 1].type == BlockType::Paragraph;
                }
                protected_lines = (int)wrapped.size();
                if (followed_by_text) {
                    protected_lines += following_gap +
                                       std::max(0, options.heading_following_lines);
                }
            }

            int protected_height = std::min(options.content_height,
                                             gap_height + protected_lines * options.line_height);
            builder.ensure_space(protected_height);
            builder.add_space(block.content_position, gap_height);

            size_t line_index = 0;
            while (line_index < wrapped.size()) {
                int capacity = (options.content_height - builder.used_height) /
                               options.line_height;
                if (capacity <= 0 && !builder.page.empty()) {
                    builder.finish_page();
                    capacity = options.content_height / options.line_height;
                }

                size_t remaining = wrapped.size() - line_index;
                int take = std::min<int>((int)remaining, std::max(1, capacity));
                int leftover = (int)remaining - take;
                int min_widow = std::max(1, options.widow_lines);
                if (leftover > 0 && leftover < min_widow && take > 1) {
                    int move = std::min(take - 1, min_widow - leftover);
                    take -= move;
                }

                for (int n = 0; n < take && line_index < wrapped.size();
                     ++n, ++line_index) {
                    builder.add_text(wrapped[line_index], block.content_position,
                                     block.heading || block.heading_level > 0);
                }
                if (line_index < wrapped.size()) builder.finish_page();
            }
        }
        pending_blank_lines = block.blank_after;
        pending_paragraph_spacing = block.paragraph_spacing_after
                                  ? options.line_height *
                                        std::max(0, options.paragraph_spacing_percent) / 100
                                  : 0;
    }

    builder.finish_page();
    if (builder.pages.empty()) builder.pages.push_back(Page());
    return builder.pages;
}

} // namespace document_layout
