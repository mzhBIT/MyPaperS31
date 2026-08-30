#include "ui/Utf8Text.h"

namespace ui::utf8 {

static size_t sequence_length(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}

bool valid(const std::string &text)
{
    for (size_t i = 0; i < text.size();) {
        size_t count = sequence_length(static_cast<unsigned char>(text[i]));
        if (count == 0 || i + count > text.size()) return false;
        for (size_t j = 1; j < count; ++j) {
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) return false;
        }
        i += count;
    }
    return true;
}

size_t length(const std::string &text)
{
    size_t result = 0;
    for (size_t i = 0; i < text.size();) {
        size_t count = sequence_length(static_cast<unsigned char>(text[i]));
        if (count == 0 || i + count > text.size()) count = 1;
        i += count;
        ++result;
    }
    return result;
}

bool erase_last(std::string *text)
{
    if (!text || text->empty()) return false;
    size_t pos = text->size() - 1;
    while (pos > 0 && (static_cast<unsigned char>((*text)[pos]) & 0xC0) == 0x80) --pos;
    text->erase(pos);
    return true;
}

std::string truncate_bytes(const std::string &text, size_t max_bytes)
{
    if (text.size() <= max_bytes) return text;
    size_t end = max_bytes;
    while (end > 0 && end < text.size() &&
           (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

bool append(std::string *text, const std::string &suffix, size_t max_bytes)
{
    if (!text || text->size() + suffix.size() > max_bytes || !valid(suffix)) return false;
    text->append(suffix);
    return true;
}

}  // namespace ui::utf8
