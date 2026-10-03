#pragma once
#include <string>

namespace calculator {
enum class Error { None, Invalid, DivideByZero, Overflow, TooLong, Domain };
struct Result { double value = 0; Error error = Error::None; };
Result evaluate(const std::string &expression, bool degrees = true);
std::string format(double value);

// UI-independent, bounded input state. No filesystem or network dependencies.
class Calculator {
public:
    bool input(char key); // digits, '.', '+', '-', '*', '/', '%', '='
    bool input_token(const std::string &token);
    void set_degrees(bool degrees);
    bool degrees() const { return degrees_; }
    void clear();
    void erase();
    void toggle_sign();
    std::string expression() const { return scientific_ ? scientific_text_ : prefix_ + operand_; }
    std::string display_value() const;
    Error error() const { return error_; }
    bool evaluated() const { return evaluated_; }
private:
    std::string prefix_, operand_;
    double result_ = 0;
    bool evaluated_ = false;
    Error error_ = Error::None;
    bool scientific_ = false, degrees_ = true;
    std::string scientific_text_;
};
// Runs against the actual core, including input editing and result chaining.
bool self_test();
} // namespace calculator
