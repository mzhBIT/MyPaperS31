#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace weread::json {

bool member(std::string_view object, std::string_view key, std::string_view *value);
bool string(std::string_view value, std::string *output);
bool integer(std::string_view value, int64_t *output);
bool boolean(std::string_view value, bool *output);

class ObjectIterator {
public:
    explicit ObjectIterator(std::string_view array);
    bool next(std::string_view *object);

private:
    std::string_view _array;
    size_t _position = 0;
};

}  // namespace weread::json
