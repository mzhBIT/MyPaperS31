#include "weread/WereadJson.h"

#include <cstdlib>

namespace weread::json {
namespace {

void skip_space(std::string_view text, size_t *position)
{
    while (*position < text.size()) {
        const char c = text[*position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++*position;
    }
}

bool quoted_end(std::string_view text, size_t start, size_t *end)
{
    if (start >= text.size() || text[start] != '"') return false;
    bool escaped = false;
    for (size_t i = start + 1; i < text.size(); ++i) {
        if (escaped) escaped = false;
        else if (text[i] == '\\') escaped = true;
        else if (text[i] == '"') { *end = i + 1; return true; }
    }
    return false;
}

bool value_end(std::string_view text, size_t start, size_t *end)
{
    if (start >= text.size()) return false;
    if (text[start] == '"') return quoted_end(text, start, end);
    if (text[start] == '{' || text[start] == '[') {
        const char open = text[start];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (size_t i = start; i < text.size(); ++i) {
            const char c = text[i];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
                continue;
            }
            if (c == '"') quoted = true;
            else if (c == open) ++depth;
            else if (c == close && --depth == 0) { *end = i + 1; return true; }
        }
        return false;
    }
    size_t i = start;
    while (i < text.size() && text[i] != ',' && text[i] != '}' && text[i] != ']') ++i;
    *end = i;
    return i > start;
}

int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string *output, uint32_t codepoint)
{
    if (codepoint <= 0x7F) output->push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7FF) {
        output->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

}  // namespace

bool member(std::string_view object, std::string_view key, std::string_view *value)
{
    if (!value) return false;
    size_t position = 0;
    skip_space(object, &position);
    if (position >= object.size() || object[position++] != '{') return false;
    while (position < object.size()) {
        skip_space(object, &position);
        if (position < object.size() && object[position] == '}') return false;
        size_t key_end = 0;
        if (!quoted_end(object, position, &key_end)) return false;
        std::string decoded_key;
        if (!string(object.substr(position, key_end - position), &decoded_key)) return false;
        position = key_end;
        skip_space(object, &position);
        if (position >= object.size() || object[position++] != ':') return false;
        skip_space(object, &position);
        size_t end = 0;
        if (!value_end(object, position, &end)) return false;
        if (decoded_key == key) {
            *value = object.substr(position, end - position);
            return true;
        }
        position = end;
        skip_space(object, &position);
        if (position < object.size() && object[position] == ',') ++position;
    }
    return false;
}

bool string(std::string_view value, std::string *output)
{
    if (!output || value.size() < 2 || value.front() != '"' || value.back() != '"') return false;
    output->clear();
    output->reserve(value.size() - 2);
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        char c = value[i];
        if (c != '\\') { output->push_back(c); continue; }
        if (++i + 1 >= value.size()) return false;
        c = value[i];
        switch (c) {
        case '"': case '\\': case '/': output->push_back(c); break;
        case 'b': output->push_back('\b'); break;
        case 'f': output->push_back('\f'); break;
        case 'n': output->push_back('\n'); break;
        case 'r': output->push_back('\r'); break;
        case 't': output->push_back('\t'); break;
        case 'u': {
            if (i + 4 >= value.size()) return false;
            uint32_t codepoint = 0;
            for (int n = 0; n < 4; ++n) {
                const int digit = hex(value[++i]);
                if (digit < 0) return false;
                codepoint = (codepoint << 4) | static_cast<uint32_t>(digit);
            }
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF && i + 6 < value.size() &&
                value[i + 1] == '\\' && value[i + 2] == 'u') {
                uint32_t low = 0;
                size_t cursor = i + 2;
                bool valid = true;
                for (int n = 0; n < 4; ++n) {
                    const int digit = hex(value[++cursor]);
                    if (digit < 0) { valid = false; break; }
                    low = (low << 4) | static_cast<uint32_t>(digit);
                }
                if (valid && low >= 0xDC00 && low <= 0xDFFF) {
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    i = cursor;
                }
            }
            append_utf8(output, codepoint);
            break;
        }
        default: return false;
        }
    }
    return true;
}

bool integer(std::string_view value, int64_t *output)
{
    if (!output || value.empty() || value.size() > 32) return false;
    std::string text(value);
    char *end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (!end || end == text.c_str()) return false;
    *output = parsed;
    return true;
}

bool boolean(std::string_view value, bool *output)
{
    if (!output) return false;
    if (value == "true") { *output = true; return true; }
    if (value == "false") { *output = false; return true; }
    int64_t number = 0;
    if (integer(value, &number)) { *output = number != 0; return true; }
    return false;
}

ObjectIterator::ObjectIterator(std::string_view array) : _array(array)
{
    while (_position < _array.size() && _array[_position] != '[') ++_position;
    if (_position < _array.size()) ++_position;
}

bool ObjectIterator::next(std::string_view *object)
{
    if (!object) return false;
    while (_position < _array.size() && _array[_position] != '{' && _array[_position] != ']') ++_position;
    if (_position >= _array.size() || _array[_position] == ']') return false;
    size_t end = 0;
    if (!value_end(_array, _position, &end)) return false;
    *object = _array.substr(_position, end - _position);
    _position = end;
    return true;
}

}  // namespace weread::json
