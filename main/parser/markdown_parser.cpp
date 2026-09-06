#include "parser/markdown_parser.h"

#include <algorithm>
#include <cctype>

namespace {

struct InlineResult {
    std::string text;
    std::vector<inline_style::Span> styles;
};

static std::string trim(const std::string &value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace((unsigned char)value[first])) ++first;
    size_t last = value.size();
    while (last > first && std::isspace((unsigned char)value[last - 1])) --last;
    return value.substr(first, last - first);
}

static bool marker_has_close(const std::string &source, size_t at, const std::string &marker)
{
    const size_t close = source.find(marker, at + marker.size());
    return close != std::string::npos && close > at + marker.size();
}

static bool underscore_delimiter(const std::string &source, size_t at, size_t count)
{
    const bool left_word = at > 0 && std::isalnum((unsigned char)source[at - 1]);
    const bool right_word = at + count < source.size() &&
                            std::isalnum((unsigned char)source[at + count]);
    return !(left_word && right_word);
}

static InlineResult parse_inline(const std::string &source, uint8_t initial_flags = inline_style::None)
{
    InlineResult result;
    uint8_t flags = initial_flags;
    size_t run_start = 0;
    uint8_t run_flags = flags;
    auto change_flags = [&](uint8_t next) {
        if (run_flags != inline_style::None && result.text.size() > run_start) {
            result.styles.push_back({run_start, result.text.size(), run_flags});
        }
        run_start = result.text.size();
        run_flags = next;
        flags = next;
    };

    for (size_t i = 0; i < source.size();) {
        if (source[i] == '\\' && i + 1 < source.size()) {
            result.text.push_back(source[i + 1]);
            i += 2;
            continue;
        }
        if (source[i] == '`') {
            size_t close = source.find('`', i + 1);
            if (close != std::string::npos) {
                const uint8_t before = flags;
                change_flags(flags | inline_style::Code);
                result.text.append(source, i + 1, close - i - 1);
                change_flags(before);
                i = close + 1;
                continue;
            }
        }
        if (source[i] == '[' || (source[i] == '!' && i + 1 < source.size() && source[i + 1] == '[')) {
            const bool image = source[i] == '!';
            const size_t label_start = i + (image ? 2 : 1);
            size_t label_end = source.find(']', label_start);
            if (label_end != std::string::npos && label_end + 1 < source.size() &&
                source[label_end + 1] == '(') {
                size_t url_end = source.find(')', label_end + 2);
                if (url_end != std::string::npos) {
                    result.text.append(source, label_start, label_end - label_start);
                    i = url_end + 1;
                    continue;
                }
            }
        }

        std::string marker;
        uint8_t mask = inline_style::None;
        if (i + 3 <= source.size() &&
            (source.compare(i, 3, "***") == 0 || source.compare(i, 3, "___") == 0)) {
            marker = source.substr(i, 3);
            mask = inline_style::Bold | inline_style::Italic;
        } else if (i + 2 <= source.size() &&
                   (source.compare(i, 2, "**") == 0 || source.compare(i, 2, "__") == 0)) {
            marker = source.substr(i, 2);
            mask = inline_style::Bold;
        } else if (source[i] == '*' || source[i] == '_') {
            marker.assign(1, source[i]);
            mask = inline_style::Italic;
        }
        if (!marker.empty() && (marker[0] != '_' || underscore_delimiter(source, i, marker.size())) &&
            ((flags & mask) != 0 || marker_has_close(source, i, marker))) {
            change_flags(flags ^ mask);
            i += marker.size();
            continue;
        }

        result.text.push_back(source[i++]);
    }
    if (run_flags != inline_style::None && result.text.size() > run_start) {
        result.styles.push_back({run_start, result.text.size(), run_flags});
    }
    return result;
}

static std::string dirname_of(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

static std::string normalize_path(const std::string &path)
{
    std::string input = path;
    std::replace(input.begin(), input.end(), '\\', '/');
    std::string prefix;
    size_t start = 0;
    if (!input.empty() && input[0] == '/') { prefix = "/"; start = 1; }
    std::vector<std::string> parts;
    while (start <= input.size()) {
        size_t slash = input.find('/', start);
        std::string part = input.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty() && part != ".") {
            if (part == ".." && !parts.empty()) parts.pop_back();
            else if (part != "..") parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out = prefix;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!out.empty() && out.back() != '/') out += '/';
        out += parts[i];
    }
    return out;
}

static bool parse_image_line(const std::string &line, const std::string &source_path,
                             EpubContentBlock *block)
{
    const std::string value = trim(line);
    if (value.rfind("![", 0) != 0) return false;
    const size_t alt_end = value.find("](", 2);
    const size_t close = value.rfind(')');
    if (alt_end == std::string::npos || close == std::string::npos || close <= alt_end + 2) return false;
    if (!trim(value.substr(close + 1)).empty()) return false;
    std::string target = trim(value.substr(alt_end + 2, close - alt_end - 2));
    if (!target.empty() && target.front() == '<' && target.back() == '>') {
        target = target.substr(1, target.size() - 2);
    } else {
        const size_t title = target.find_first_of(" \t");
        if (title != std::string::npos) target.erase(title);
    }
    if (target.empty() || target.find("://") != std::string::npos || target.rfind("data:", 0) == 0) return false;
    if (target.front() != '/') target = dirname_of(source_path) + "/" + target;
    block->type = EpubContentBlockType::Image;
    block->href = normalize_path(target);
    block->blank_after = 1;
    return true;
}

static bool horizontal_rule(const std::string &line)
{
    std::string compact;
    for (char c : trim(line)) if (c != ' ' && c != '\t') compact.push_back(c);
    if (compact.size() < 3) return false;
    return std::all_of(compact.begin(), compact.end(), [&](char c) { return c == compact[0]; }) &&
           (compact[0] == '-' || compact[0] == '*' || compact[0] == '_');
}

static void append_text_block(std::vector<EpubContentBlock> *blocks,
                              const std::string &source, bool heading = false,
                              int heading_level = 0, uint8_t initial_style = inline_style::None)
{
    InlineResult parsed = parse_inline(trim(source), initial_style);
    if (parsed.text.empty()) return;
    EpubContentBlock block;
    block.type = EpubContentBlockType::Text;
    block.text = std::move(parsed.text);
    block.styles = std::move(parsed.styles);
    block.heading = heading;
    block.heading_level = heading_level;
    block.blank_before = heading ? 1 : 0;
    block.blank_after = 1;
    blocks->push_back(std::move(block));
}

} // namespace

bool markdown_parse_blocks(const std::string &source, const std::string &source_path,
                           std::vector<EpubContentBlock> *blocks,
                           std::vector<MarkdownTocItem> *toc)
{
    if (!blocks) return false;
    blocks->clear();
    if (toc) toc->clear();

    std::string paragraph;
    bool fenced = false;
    char fence_char = 0;
    size_t fence_len = 0;
    std::string code;
    auto flush_paragraph = [&]() {
        if (!paragraph.empty()) append_text_block(blocks, paragraph);
        paragraph.clear();
    };

    size_t pos = 0;
    while (pos <= source.size()) {
        size_t end = source.find('\n', pos);
        std::string line = source.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = end == std::string::npos ? source.size() + 1 : end + 1;

        const std::string stripped = trim(line);
        size_t marker_count = 0;
        while (marker_count < stripped.size() &&
               (stripped[marker_count] == '`' || stripped[marker_count] == '~') &&
               stripped[marker_count] == stripped[0]) ++marker_count;
        if (marker_count >= 3) {
            if (!fenced) {
                flush_paragraph();
                fenced = true;
                fence_char = stripped[0];
                fence_len = marker_count;
                code.clear();
            } else if (stripped[0] == fence_char && marker_count >= fence_len) {
                append_text_block(blocks, code, false, 0, inline_style::Code);
                fenced = false;
                code.clear();
            }
            continue;
        }
        if (fenced) {
            if (!code.empty()) code += '\n';
            code += line;
            continue;
        }
        if (stripped.empty()) {
            flush_paragraph();
            continue;
        }

        size_t hashes = 0;
        while (hashes < stripped.size() && stripped[hashes] == '#') ++hashes;
        if (hashes > 0 && hashes <= 6 && hashes < stripped.size() && stripped[hashes] == ' ') {
            flush_paragraph();
            std::string heading_text = trim(stripped.substr(hashes + 1));
            while (!heading_text.empty() && heading_text.back() == '#') heading_text.pop_back();
            heading_text = trim(heading_text);
            append_text_block(blocks, heading_text, true, (int)hashes, inline_style::Bold);
            if (toc && !blocks->empty()) toc->push_back({blocks->back().text, (int)hashes - 1});
            continue;
        }
        if (horizontal_rule(stripped)) {
            flush_paragraph();
            EpubContentBlock block;
            block.type = EpubContentBlockType::HorizontalRule;
            blocks->push_back(std::move(block));
            continue;
        }
        EpubContentBlock image;
        if (parse_image_line(stripped, source_path, &image)) {
            flush_paragraph();
            blocks->push_back(std::move(image));
            continue;
        }

        std::string text = stripped;
        uint8_t initial_style = inline_style::None;
        if (text.rfind(">", 0) == 0) {
            flush_paragraph();
            text = "│ " + trim(text.substr(1));
            initial_style = inline_style::Italic;
            append_text_block(blocks, text, false, 0, initial_style);
            continue;
        }
        if (text.size() >= 2 && (text[0] == '-' || text[0] == '*' || text[0] == '+') && text[1] == ' ') {
            flush_paragraph();
            append_text_block(blocks, "• " + trim(text.substr(2)));
            continue;
        }
        size_t digit = 0;
        while (digit < text.size() && std::isdigit((unsigned char)text[digit])) ++digit;
        if (digit > 0 && digit + 1 < text.size() && text[digit] == '.' && text[digit + 1] == ' ') {
            flush_paragraph();
            append_text_block(blocks, text.substr(0, digit + 1) + " " + trim(text.substr(digit + 2)));
            continue;
        }

        if (!paragraph.empty()) paragraph += ' ';
        paragraph += stripped;
    }
    if (fenced && !code.empty()) append_text_block(blocks, code, false, 0, inline_style::Code);
    flush_paragraph();
    return !blocks->empty();
}
