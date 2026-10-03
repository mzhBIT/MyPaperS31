#include "calculator/Calculator.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

namespace calculator {
namespace {
constexpr size_t MAX_EXPRESSION = 256;
constexpr size_t MAX_DIGITS = 15;
bool digit(char c) { return c >= '0' && c <= '9'; }
bool operation(char c) { return c == '+' || c == '-' || c == '*' || c == '/'; }
std::string precise(double value) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    return buf;
}
// Locate the final primary (number, constant, parenthesized expression/function).
size_t last_value_start(const std::string &text) {
    size_t start = text.size();
    if (start && text[start-1] == '%') --start;
    if (start && text[start-1] == ')') {
        int balance = 0;
        do {
            --start;
            if (text[start] == ')') ++balance;
            if (text[start] == '(') --balance;
        } while (start && balance);
        while (start && text[start-1] >= 'a' && text[start-1] <= 'z') --start;
    } else {
        while (start) {
            char c = text[start-1];
            if (digit(c) || c == '.' || (c >= 'a' && c <= 'z') || c == 'E') --start;
            else if ((c == '-' || c == '+') && start >= 2 &&
                     (text[start-2] == 'e' || text[start-2] == 'E')) --start;
            else break;
        }
    }
    if (start && (text[start-1] == '-' || text[start-1] == '+') &&
        (start == 1 || operation(text[start-2]) || text[start-2] == '^' || text[start-2] == '(')) --start;
    return start;
}

class Parser {
    const char *cursor_;
    Error error_ = Error::None;
    bool degrees_;
    int depth_ = 0;
    static constexpr double PI = 3.14159265358979323846;
    void spaces() { while (*cursor_ == ' ') ++cursor_; }
    double checked(double value) {
        if (std::isnan(value)) error_ = Error::Domain;
        else if (!std::isfinite(value)) error_ = Error::Overflow;
        return value;
    }
    double primary() {
        spaces();
        if (*cursor_ == '(') {
            ++cursor_;
            double value = sum();
            spaces();
            if (*cursor_ != ')') { error_ = Error::Invalid; return 0; }
            ++cursor_; return value;
        }
        if (*cursor_ >= 'a' && *cursor_ <= 'z') {
            const char *start = cursor_;
            while (*cursor_ >= 'a' && *cursor_ <= 'z') ++cursor_;
            std::string name(start, cursor_-start);
            if (name == "pi") return PI;
            if (name == "e") return std::exp(1.0);
            spaces();
            if (*cursor_ != '(') { error_ = Error::Invalid; return 0; }
            ++cursor_;
            double arg = sum(); spaces();
            if (*cursor_ != ')') { error_ = Error::Invalid; return 0; }
            ++cursor_;
            if (error_ != Error::None) return 0;
            const double angle = degrees_ ? arg*PI/180.0 : arg;
            const double inverse_scale = degrees_ ? 180.0/PI : 1.0;
            if (name == "sin") return checked(std::sin(angle));
            if (name == "cos") return checked(std::cos(angle));
            if (name == "tan") {
                if (std::abs(std::cos(angle)) < 1e-14) { error_ = Error::Domain; return 0; }
                return checked(std::tan(angle));
            }
            if (name == "asin" || name == "acos") {
                if (std::abs(arg) > 1) { error_ = Error::Domain; return 0; }
                return checked((name == "asin" ? std::asin(arg) : std::acos(arg))*inverse_scale);
            }
            if (name == "atan") return checked(std::atan(arg)*inverse_scale);
            if (name == "sqrt") {
                if (arg < 0) { error_ = Error::Domain; return 0; }
                return std::sqrt(arg);
            }
            if (name == "ln" || name == "log") {
                if (arg <= 0) { error_ = Error::Domain; return 0; }
                return name == "ln" ? std::log(arg) : std::log10(arg);
            }
            if (name == "exp") return checked(std::exp(arg));
            if (name == "powten") return checked(std::pow(10.0,arg));
            if (name == "abs") return std::abs(arg);
            if (name == "inv") {
                if (arg == 0) { error_ = Error::DivideByZero; return 0; }
                return checked(1.0/arg);
            }
            error_ = Error::Invalid; return 0;
        }
        if (!digit(*cursor_) && *cursor_ != '.') { error_ = Error::Invalid; return 0; }
        const char *start = cursor_;
        char *end = nullptr;
        errno = 0;
        double value = std::strtod(start, &end);
        if (end == start) { error_ = Error::Invalid; return 0; }
        cursor_ = end;
        if (errno == ERANGE || !std::isfinite(value)) { error_ = Error::Overflow; return 0; }
        return value;
    }
    double power() {
        double value = primary(); spaces();
        if (*cursor_ == '%') { ++cursor_; value /= 100.0; spaces(); }
        if (error_ == Error::None && *cursor_ == '^') {
            ++cursor_; double exponent = unary();
            if (error_ != Error::None) return 0;
            if (value == 0 && exponent < 0) { error_ = Error::DivideByZero; return 0; }
            value = checked(std::pow(value,exponent));
        }
        return value;
    }
    double unary() {
        if (++depth_ > 32) { --depth_; error_ = Error::Invalid; return 0; }
        spaces(); double value;
        if (*cursor_ == '+' || *cursor_ == '-') {
            char sign = *cursor_++; value = unary(); if (sign == '-') value = -value;
        } else value = power();
        --depth_; return value;
    }
    double term() {
        double value = unary();
        spaces();
        while (error_ == Error::None && (*cursor_ == '*' || *cursor_ == '/')) {
            char op = *cursor_++;
            double right = unary();
            if (error_ != Error::None) break;
            if (op == '/' && right == 0) { error_ = Error::DivideByZero; break; }
            value = checked(op == '*' ? value * right : value / right);
            spaces();
        }
        return value;
    }
    double sum() {
        double value = term();
        spaces();
        while (error_ == Error::None && (*cursor_ == '+' || *cursor_ == '-')) {
            char op = *cursor_++;
            double right = term();
            if (error_ != Error::None) break;
            value = checked(op == '+' ? value + right : value - right);
            spaces();
        }
        return value;
    }
public:
    explicit Parser(const char *text, bool degrees) : cursor_(text), degrees_(degrees) {}
    Result run() {
        double value = sum(); spaces();
        if (error_ == Error::None && *cursor_) error_ = Error::Invalid;
        return {value, error_};
    }
};
}

Result evaluate(const std::string &expression, bool degrees) {
    if (expression.size() > MAX_EXPRESSION) return {0, Error::TooLong};
    if (expression.empty()) return {0, Error::Invalid};
    if (expression.find('\0') != std::string::npos) return {0, Error::Invalid};
    return Parser(expression.c_str(), degrees).run();
}
std::string format(double value) {
    if (value == 0) return "0"; // No negative zero or arbitrary small-value cutoff.
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.12g", value);
    return buf;
}
void Calculator::clear() {
    prefix_.clear(); operand_.clear(); result_ = 0;
    scientific_text_.clear(); scientific_ = false;
    evaluated_ = false; error_ = Error::None;
}
void Calculator::set_degrees(bool degrees) {
    if (degrees_ == degrees) return;
    degrees_ = degrees;
    const auto text = expression();
    if (text.find("sin(") != std::string::npos || text.find("cos(") != std::string::npos ||
        text.find("tan(") != std::string::npos) evaluated_ = false;
    error_ = Error::None;
}
std::string Calculator::display_value() const {
    if (evaluated_) return format(result_);
    if (scientific_) {
        if (scientific_text_.empty()) return "0";
        auto preview = evaluate(scientific_text_,degrees_);
        return preview.error == Error::None ? format(preview.value) : "...";
    }
    return operand_.empty() ? "0" : operand_;
}
void Calculator::erase() {
    if (scientific_) {
        if (evaluated_) {
            scientific_text_ = format(result_); evaluated_ = false;
            if (scientific_text_.find_first_of("eE") != std::string::npos) scientific_text_.clear();
            else if (!scientific_text_.empty()) scientific_text_.pop_back();
        } else if (!scientific_text_.empty()) {
            bool function_start = scientific_text_.back() == '(';
            scientific_text_.pop_back();
            if (function_start) while (!scientific_text_.empty() &&
                scientific_text_.back() >= 'a' && scientific_text_.back() <= 'z') scientific_text_.pop_back();
        }
        error_ = Error::None; return;
    }
    if (evaluated_) {
        operand_ = format(result_); prefix_.clear(); evaluated_ = false;
        // A scientific-notation result is atomic when edited.
        if (operand_.find_first_of("eE") != std::string::npos) operand_.clear();
        else if (!operand_.empty()) operand_.pop_back();
    } else if (!operand_.empty()) operand_.pop_back();
    else if (!prefix_.empty()) {
        prefix_.pop_back(); // Remove the pending binary operator.
        size_t begin = 0;
        // Find the last binary operator, not a unary sign/exponent sign.
        for (size_t i = 1; i < prefix_.size(); ++i) {
            if (operation(prefix_[i]) && prefix_[i-1] != 'e' && prefix_[i-1] != 'E' &&
                !operation(prefix_[i-1])) begin = i + 1;
        }
        operand_ = prefix_.substr(begin); prefix_.resize(begin);
    }
    error_ = Error::None;
}
void Calculator::toggle_sign() {
    if (scientific_) {
        std::string text = evaluated_ ? precise(result_) : scientific_text_;
        if (text.size()+3 > MAX_EXPRESSION) { error_ = Error::TooLong; return; }
        size_t start = last_value_start(text);
        scientific_text_ = text.substr(0,start)+"-("+text.substr(start)+")";
        evaluated_ = false; error_ = Error::None; return;
    }
    if (evaluated_) {
        result_ = -result_; prefix_.clear(); operand_ = precise(result_);
        evaluated_ = false;
    } else if (!operand_.empty() && operand_[0] == '-') operand_.erase(0, 1);
    else if (expression().size() < MAX_EXPRESSION) operand_.insert(0, "-");
    error_ = Error::None;
}
bool Calculator::input(char key) {
    error_ = Error::None;
    if (key == '=') {
        if (evaluated_) return false;
        auto result = evaluate(expression(),degrees_);
        error_ = result.error;
        if (error_ != Error::None) return false;
        result_ = result.value; evaluated_ = true; return true;
    }
    if (scientific_) return input_token(std::string(1,key));
    if (digit(key) || key == '.') {
        if (evaluated_) clear();
        if (!operand_.empty() && (operand_.back() == '%' || operand_.find_first_of("eE") != std::string::npos)) operand_.clear();
        if (expression().size() >= MAX_EXPRESSION) { error_ = Error::TooLong; return false; }
        if (key == '.') {
            if (operand_.find('.') != std::string::npos) return false;
            if (operand_.empty() || operand_ == "-") {
                if (expression().size()+2 > MAX_EXPRESSION) { error_ = Error::TooLong; return false; }
                operand_ += '0';
            }
        } else {
            size_t digits = 0; for (char c : operand_) if (digit(c)) ++digits;
            if (digits >= MAX_DIGITS) { error_ = Error::TooLong; return false; }
            if (operand_ == "0") operand_.clear();
            else if (operand_ == "-0") operand_ = "-";
        }
        operand_ += key; return true;
    }
    if (key == '%') {
        if (evaluated_) { prefix_.clear(); operand_ = precise(result_); evaluated_ = false; }
        if (operand_.empty() || operand_ == "-" || operand_.back() == '%') return false;
        if (expression().size() >= MAX_EXPRESSION) { error_ = Error::TooLong; return false; }
        operand_ += '%'; return true;
    }
    if (operation(key)) {
        if (evaluated_) {
            // Preserve full double precision when continuing a calculation.
            operand_ = precise(result_); prefix_.clear(); evaluated_ = false;
        }
        if (operand_.empty() || operand_ == "-") {
            if (key == '-') {
                if (operand_.empty() && expression().size() >= MAX_EXPRESSION) {
                    error_ = Error::TooLong; return false;
                }
                operand_ = "-"; return true;
            }
            if (!prefix_.empty()) { prefix_.back() = key; operand_.clear(); return true; }
            return false;
        }
        if (expression().size() >= MAX_EXPRESSION) { error_ = Error::TooLong; return false; }
        prefix_ += operand_; prefix_ += key; operand_.clear(); return true;
    }
    return false;
}

bool Calculator::input_token(const std::string &token) {
    if (token == "=") return input('=');
    const bool function = token == "sin(" || token == "cos(" || token == "tan(" ||
        token == "asin(" || token == "acos(" || token == "atan(" || token == "sqrt(" ||
        token == "ln(" || token == "log(" || token == "exp(" || token == "powten(" ||
        token == "abs(" || token == "inv(";
    const bool constant = token == "pi" || token == "e";
    const bool numeric = token.size() == 1 && (digit(token[0]) || token[0] == '.');
    const bool binary = token.size() == 1 && (operation(token[0]) || token[0] == '^');
    if (!function && !constant && !numeric && !binary && token != "(" && token != ")" &&
        token != "%" && token != "^2" && token != "^3") return false;
    error_ = Error::None;
    std::string text = evaluated_ ? precise(result_) : expression();
    if (evaluated_ && (numeric || constant || token == "(")) text.clear();
    const auto complete = [](const std::string &s) {
        return !s.empty() && (digit(s.back()) || s.back() == '.' || s.back() == ')' ||
                             s.back() == '%' || s.back() == 'i' || s.back() == 'e');
    };
    if (function && complete(text)) {
        size_t start = evaluated_ ? 0 : last_value_start(text);
        text = text.substr(0,start)+token+text.substr(start)+")";
    }
    else if (function || constant || token == "(") {
        if (complete(text)) text += '*';
        text += token;
    } else if (numeric) {
        if (!text.empty() && (text.back() == ')' || text.back() == 'i' || text.back() == 'e')) text += '*';
        if (!text.empty() && text.back() == '%') return false;
        size_t start = text.size();
        while (start && (digit(text[start-1]) || text[start-1] == '.')) --start;
        const auto number = text.substr(start);
        if (token == ".") {
            if (number.find('.') != std::string::npos) return false;
            if (number.empty()) text += '0';
        } else {
            size_t count = 0; for (char c : number) if (digit(c)) ++count;
            if (count >= MAX_DIGITS) { error_ = Error::TooLong; return false; }
            if (number == "0") text.resize(start);
        }
        text += token;
    } else if (token == ")") {
        int balance = 0; for (char c : text) { if (c == '(') ++balance; if (c == ')') --balance; }
        if (balance <= 0 || !complete(text)) return false;
        text += token;
    } else if (binary) {
        if (!complete(text)) {
            if (token == "-") text += token;
            else if (!text.empty() && (operation(text.back()) || text.back() == '^')) text.back() = token[0];
            else return false;
        } else text += token;
    } else {
        if (!complete(text) || text.back() == '%') return false;
        text += token;
    }
    if (text.size() > MAX_EXPRESSION) { error_ = Error::TooLong; return false; }
    scientific_text_ = text; scientific_ = true; evaluated_ = false;
    prefix_.clear(); operand_.clear(); return true;
}

bool self_test() {
    struct Case { const char *text; double value; Error error; };
    const Case cases[] = {
        {"2+3*4",14,Error::None},{"10-8/2",6,Error::None},
        {"2*-3",-6,Error::None},{"-2--3",1,Error::None},
        {"200*10%",20,Error::None},{"0.1+0.2",0.3,Error::None},
        {"1e-20*2",2e-20,Error::None},{"1/0",0,Error::DivideByZero},
        {"1+",0,Error::Invalid},{".",0,Error::Invalid},
        {"1e309",0,Error::Overflow},{"1e308*10",0,Error::Overflow},
        {"nan",0,Error::Invalid},{"2%%",0,Error::Invalid},
        {"(2+3)*4",20,Error::None},{"2^3^2",512,Error::None},
        {"-2^2",-4,Error::None},{"2^-3",0.125,Error::None},
        {"sin(30)",0.5,Error::None},{"asin(0.5)",30,Error::None},
        {"sqrt(9)+log(100)+ln(e)",6,Error::None},
        {"sqrt(-1)",0,Error::Domain},{"tan(90)",0,Error::Domain},
    };
    for (const auto &c : cases) {
        auto r = evaluate(c.text);
        if (r.error != c.error) return false;
        if (c.error == Error::None && std::abs(r.value-c.value) > std::abs(c.value)*1e-12) return false;
    }
    auto radians = evaluate("sin(pi/2)",false);
    if (radians.error != Error::None || std::abs(radians.value-1) > 1e-12) return false;
    Calculator c;
    for (char key : std::string("2+3*4=")) c.input(key);
    if (c.display_value() != "14") return false;
    for (char key : std::string("/2=")) c.input(key);
    if (c.display_value() != "7") return false;
    c.input('9'); if (c.expression() != "9") return false;
    c.toggle_sign(); if (c.expression() != "-9") return false;
    c.clear(); for (char key : std::string("12+3*")) c.input(key);
    c.erase(); if (c.expression() != "12+3") return false;
    c.erase(); if (c.expression() != "12+") return false;
    c.clear(); c.input('.'); c.input('5'); c.input('=');
    if (c.display_value() != "0.5") return false;
    c.clear(); for (char key : std::string("123=")) c.input(key);
    c.erase(); if (c.expression() != "12") return false;
    c.clear(); for (char key : std::string("200*10%=")) c.input(key);
    if (c.display_value() != "20") return false;
    c.clear(); for (char key : std::string("1/3=*3=")) c.input(key);
    return c.display_value() == "1" && format(0.1+0.2) == "0.3";
}
} // namespace calculator
