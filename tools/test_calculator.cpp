#include "calculator/Calculator.h"
#include "calculator/CalculatorPresentation.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <algorithm>

int main() {
    assert(calculator::function_label("(")=="(");
    assert(calculator::function_label(")")==")");
    assert(calculator::function_label("sin(")=="sin");
    assert(calculator::function_label("asin(")=="asin");
    assert(calculator::function_label("").empty());
    // Scientific keypad starts at 256: old y=171 overflows, new y=145 fits.
    assert(!calculator::result_fits(200,98,652,256-12-171));
    assert(calculator::result_fits(200,98,652,256-12-145));
    assert(!calculator::result_fits(200,104,652,256-12-148));
    assert(!calculator::result_fits(700,98,652,99));
    assert(calculator::self_test());
    calculator::Calculator c;
    auto keys = [&](const char *text) { for (const char *p=text; *p; ++p) c.input(*p); };
    keys("5*-2="); assert(c.display_value()=="-10");
    keys("+3="); assert(c.display_value()=="-7");
    c.clear(); keys("9/0="); assert(c.error()==calculator::Error::DivideByZero);
    c.erase(); keys("3="); assert(c.display_value()=="3");
    c.clear(); keys("12+34*"); c.erase(); assert(c.expression()=="12+34");
    c.erase(); assert(c.expression()=="12+3");
    c.clear(); keys("12+-3="); assert(c.display_value()=="9");
    c.clear(); keys("1.2.3="); assert(c.display_value()=="1.23");
    c.clear(); keys("200*10%="); assert(c.display_value()=="20");
    assert(calculator::evaluate(std::string(257,'1')).error==calculator::Error::TooLong);
    c.clear(); keys("1234567890123456"); assert(c.error()==calculator::Error::TooLong);
    assert(c.expression()=="123456789012345");
    c.clear();
    for (int i=0; i<128; ++i) keys("1+");
    assert(c.expression().size()==256);
    c.input('-'); assert(c.error()==calculator::Error::TooLong);
    assert(c.expression().size()==256);
    c.erase(); assert(c.expression().size()==255);
    c.clear(); keys("0.1+0.2=");
    c.toggle_sign(); keys("+0.3=");
    assert(std::abs(calculator::evaluate(c.display_value()).value)<1e-15);
    const auto check = [](const char *text, double value, bool degrees = true) {
        auto r = calculator::evaluate(text,degrees);
        assert(r.error == calculator::Error::None);
        assert(std::abs(r.value-value) <= 1e-10*std::max(1.0,std::abs(value)));
    };
    check("(2+3)*4",20); check("2^3^2",512); check("-2^2",-4);
    check("(-2)^2",4); check("2^-3",0.125);
    check("sin(30)",0.5); check("cos(60)",0.5); check("tan(45)",1);
    check("sin(pi/2)",1,false); check("asin(0.5)",30);
    check("acos(0)",90); check("atan(1)",45);
    check("sqrt(9)+log(100)+ln(e)",6);
    check("exp(0)+powten(2)+inv(4)+abs(-2)",103.25);
    assert(calculator::evaluate("sqrt(-1)").error==calculator::Error::Domain);
    assert(calculator::evaluate("ln(0)").error==calculator::Error::Domain);
    assert(calculator::evaluate("asin(2)").error==calculator::Error::Domain);
    assert(calculator::evaluate("tan(90)").error==calculator::Error::Domain);
    assert(calculator::evaluate("(-2)^0.5").error==calculator::Error::Domain);
    assert(calculator::evaluate("inv(0)").error==calculator::Error::DivideByZero);
    assert(calculator::evaluate("(1+2").error==calculator::Error::Invalid);
    assert(calculator::evaluate("sin").error==calculator::Error::Invalid);
    assert(calculator::evaluate(std::string(40,'(')+"1"+std::string(40,')')).error==calculator::Error::Invalid);
    c.clear(); c.input_token("sin("); keys("30"); c.input_token(")"); keys("=");
    assert(c.display_value()=="0.5");
    c.input_token("asin("); keys("="); assert(c.display_value()=="30");
    c.clear(); keys("2+"); c.input_token("("); keys("3*4"); c.input_token(")"); keys("=");
    assert(c.display_value()=="14");
    c.clear(); keys("2"); c.input_token("pi"); keys("=");
    check(c.display_value().c_str(),2*3.141592653589793);
    c.clear(); c.input_token("ln("); c.erase(); assert(c.expression().empty());
    c.clear(); c.input_token("e"); c.erase(); assert(c.expression().empty());
    c.clear(); keys("2+9"); c.input_token("sqrt("); keys("="); assert(c.display_value()=="5");
    c.clear(); keys("-9"); c.input_token("sqrt("); keys("="); assert(c.error()==calculator::Error::Domain);
    c.clear(); keys("2+"); c.input_token("("); keys("3+6"); c.input_token(")");
    c.input_token("sqrt("); keys("="); assert(c.display_value()=="5");
    c.clear(); c.input_token("("); keys("1"); c.input_token(")"); keys("+2"); c.toggle_sign();
    keys("="); assert(c.display_value()=="-1");
    c.clear(); c.input_token("sin("); keys("30"); c.input_token(")"); keys("=");
    c.set_degrees(false); assert(!c.evaluated()); keys("=");
    check(c.display_value().c_str(),std::sin(30.0));
    c.set_degrees(true);
    // Deterministic rapid input/edit sequences must never exceed the bound.
    unsigned seed = 1;
    const char *tokens[] = {"(", ")", "sin(", "sqrt(", "^", "^2", "pi", "e", "ln("};
    for (int i=0; i<10000; ++i) {
        seed = seed*1664525u+1013904223u;
        switch (seed%7) {
        case 0: c.clear(); break;
        case 1: c.erase(); break;
        case 2: c.toggle_sign(); break;
        case 3: c.input_token(tokens[(seed/7)%9]); break;
        case 4: c.input('='); break;
        default: c.input("0123456789.+-*/%"[(seed/7)%15]); break;
        }
        assert(c.expression().size() <= 256);
        (void)c.display_value();
    }
    std::puts("calculator tests passed");
}
