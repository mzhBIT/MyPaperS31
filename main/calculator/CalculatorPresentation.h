#pragma once
#include <string>

namespace calculator {
// Function tokens include an opening parenthesis; the standalone key does not.
inline std::string function_label(const std::string &token) {
    return token.size() > 1 && token.back() == '(' ? token.substr(0,token.size()-1) : token;
}
inline bool result_fits(int width, int height, int available_width, int available_height) {
    return width <= available_width && height <= available_height;
}
} // namespace calculator
