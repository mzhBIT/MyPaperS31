#include "activities/CalculatorActivity.h"
#include "activities/StatusBarActivity.h"
#include "activities/ActivityManager.h"
#include "calculator/Calculator.h"
#include "calculator/CalculatorPresentation.h"
#include "calculator/CalculatorLayout.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"
#include "ui/calculator_icons_1bpp.h"
#include <M5Unified.h>
#include <esp_log.h>
#include <algorithm>
#include <vector>
#include <string>

namespace {
using ui_locale::TextId;
const char *tr(TextId id) { return ui_locale::text(id); }
struct Entry { std::string expression, result; };
std::vector<Entry> history; // At most 20; survives closing the app, not reboot.
constexpr int PAD = calculator::layout::padding, GAP = calculator::layout::gap, FULL_REFRESH_INTERVAL = 30;
constexpr int CONTENT_TOP = calculator::layout::content_top;
constexpr char KEYS[] = {'C','D','%','/', '7','8','9','*', '4','5','6','-',
                         '1','2','3','+', 'S','0','.','='};
constexpr const char *SCI_KEYS[] = {"(", ")", "^2", "^", "sqrt(", "ln(", "log(", "2nd",
                                  "sin(", "cos(", "tan(", "angle", "pi", "e", "abs(", "inv("};
constexpr const char *SECOND_KEYS[] = {"(", ")", "^3", "^", "sqrt(", "exp(", "powten(", "2nd",
                                     "asin(", "acos(", "atan(", "angle", "pi", "e", "abs(", "inv("};

std::string pretty(const std::string &expression) {
    std::string out;
    for (char c : expression) {
        if (c == '*') out += " × ";
        else if (c == '/') out += " ÷ ";
        else if (c == '+') out += " + ";
        else if (c == '-') out += "−";
        else out += c;
    }
    return out;
}
std::string tail(const std::string &text, int width) {
    if (system_font::measure(text) <= width) return text;
    size_t start = 0;
    while (start < text.size() && system_font::measure("..." + text.substr(start)) > width) {
        ++start;
        while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) ++start;
    }
    return "..." + text.substr(start);
}
TextId error_text(calculator::Error error) {
    switch (error) {
    case calculator::Error::DivideByZero: return TextId::CalcDivideByZero;
    case calculator::Error::Overflow: return TextId::CalcOverflow;
    case calculator::Error::TooLong: return TextId::CalcTooLong;
    case calculator::Error::Domain: return TextId::CalcDomain;
    default: return TextId::CalcInvalid;
    }
}

class CalculatorActivity final : public StatusBarActivity {
    calculator::Calculator calc_;
    bool dirty_ = true, full_ = true, history_view_ = false, exiting_ = false;
    bool core_ok_ = true;
    bool scientific_ = false, second_ = false;
    int partial_count_ = 0, history_page_ = 0, focus_ = -1;
    using Geometry = calculator::layout::Geometry;
    Geometry geometry() const {
        return calculator::layout::geometry(M5.Display.width(),M5.Display.height(),scientific_);
    }
    int key_count() const { return scientific_ ? 36 : 20; }
    void toggle_mode() { scientific_ = !scientific_; focus_ = -1; dirty_ = full_ = true; }
    std::string key_label(int index) const {
        if (scientific_ && index < 16) {
            const std::string token = (second_ ? SECOND_KEYS : SCI_KEYS)[index];
            if (token == "angle") return calc_.degrees() ? "DEG" : "RAD";
            if (token == "2nd") return second_ ? "2nd*" : "2nd";
            if (token == "^2") return "x²";
            if (token == "^3") return "x³";
            if (token == "^") return "x^y";
            if (token == "sqrt(") return "√";
            if (token == "powten(") return "10^x";
            if (token == "exp(") return "e^x";
            if (token == "pi") return "π";
            if (token == "inv(") return "1/x";
            return calculator::function_label(token);
        }
        const char key = KEYS[index-(scientific_ ? 16 : 0)];
        switch (key) {
        case 'C': return "AC";
        case 'D': return tr(TextId::Delete);
        case 'S': return "±";
        case '/': return "÷";
        case '*': return "×";
        case '-': return "−";
        default: return std::string(1,key);
        }
    }
    int history_rows() const {
        return std::max(1, (static_cast<int>(M5.Display.height())-CONTENT_TOP-72)/132);
    }
    int history_pages() const {
        return std::max(1, (static_cast<int>(history.size())+history_rows()-1)/history_rows());
    }
    void page(int delta) {
        const int target = std::clamp(history_page_+delta, 0, history_pages()-1);
        if (target == history_page_) return;
        history_page_ = target;
        dirty_ = full_ = true;
    }
    void back() {
        if (history_view_) { history_view_ = false; dirty_ = full_ = true; }
        else exiting_ = true;
    }
    void activate(int index) {
        if (!core_ok_ || index < 0 || index >= key_count()) return;
        if (scientific_ && index < 16) {
            const std::string token = (second_ ? SECOND_KEYS : SCI_KEYS)[index];
            if (token == "2nd") { second_ = !second_; full_ = true; }
            else if (token == "angle") { calc_.set_degrees(!calc_.degrees()); full_ = true; }
            else calc_.input_token(token);
            dirty_ = true; return;
        }
        const auto before = calc_.expression();
        const auto before_error = calc_.error();
        const bool before_evaluated = calc_.evaluated();
        const char key = KEYS[index-(scientific_ ? 16 : 0)];
        if (key == 'C') calc_.clear();
        else if (key == 'D') calc_.erase();
        else if (key == 'S') calc_.toggle_sign();
        else {
            const std::string expression = calc_.expression();
            if (calc_.input(key) && key == '=') {
                if (history.size() == 20) history.erase(history.begin());
                const bool trig = expression.find("sin(") != std::string::npos ||
                    expression.find("cos(") != std::string::npos || expression.find("tan(") != std::string::npos;
                history.push_back({expression + (trig ? (calc_.degrees() ? " [DEG]" : " [RAD]") : ""), calc_.display_value()});
            }
        }
        if (before != calc_.expression() || before_error != calc_.error() ||
            before_evaluated != calc_.evaluated()) dirty_ = true;
    }
    void draw_actions(int width) {
        auto& d=M5.Display;
        const int pixels=std::min(32,system_font::pixel_size());
        for (int side=0; side<(history_view_ ? 1 : 2); ++side) {
            const auto box=calculator::layout::action(width,side!=0);
            const std::string label=history_view_ ? std::string("< ")+tr(TextId::Calculator)
                : tr(side ? TextId::CalcHistory : scientific_ ? TextId::CalcBasic : TextId::CalcScientific);
            const auto fitted=app_status_bar::fit(label,box.w-12,pixels);
            system_font::draw_on_background(d,fitted,
                box.x+(box.w-system_font::measure(fitted,pixels))/2,
                system_font::centered_y(fitted,box.y+box.h/2,pixels),pixels,TFT_BLACK,TFT_WHITE);
        }
        d.drawFastHLine(0,CONTENT_TOP-1,width,TFT_BLACK);
    }
    void draw_result(const Geometry &g) {
        auto &d = M5.Display;
        int32_t clip_x, clip_y, clip_w, clip_h;
        d.getClipRect(&clip_x,&clip_y,&clip_w,&clip_h);
        const int x = std::max(0,static_cast<int>(clip_x));
        const int y = std::max(CONTENT_TOP,static_cast<int>(clip_y));
        const int right = std::min(g.width,static_cast<int>(clip_x+clip_w));
        const int bottom = std::min(g.keypad_y,static_cast<int>(clip_y+clip_h));
        // Never allow glyph backgrounds to erase the first row of keys.
        d.setClipRect(x,y,std::max(0,right-x),std::max(0,bottom-y));
        d.fillRect(0, CONTENT_TOP, g.width, g.keypad_y-CONTENT_TOP, TFT_WHITE);
        const int font_h = system_font::line_height();
        const int expression_y = CONTENT_TOP+12;
        const int result_y = expression_y+font_h+8;
        system_font::draw_right(d, tail(pretty(calc_.expression()), g.width-2*PAD),
                                g.width-PAD, expression_y);
        if (!core_ok_ || calc_.error() != calculator::Error::None) {
            system_font::draw_right(d, tr(core_ok_ ? error_text(calc_.error()) : TextId::CalcSelfTestFailed),
                                    g.width-PAD, result_y);
        } else {
            const std::string value = calc_.display_value();
            const int large_px = system_font::pixel_size() * 2;
            const bool large = calculator::result_fits(
                system_font::measure(value, large_px),
                system_font::line_height(large_px),
                g.width-2*PAD, g.keypad_y-GAP-result_y);
            system_font::draw_right_sized(d, value, g.width-PAD, result_y,
                                          large ? large_px : system_font::pixel_size());
        }
        d.setClipRect(clip_x,clip_y,clip_w,clip_h);
    }
    void draw_keyboard(const Geometry &g) {
        auto &d = M5.Display;
        const int font_h = system_font::line_height();
        for (int i=0; i<key_count(); ++i) {
            const int x = PAD+(i%4)*(g.key_width+GAP), y = g.keypad_y+(i/4)*(g.key_height+GAP);
            d.drawRoundRect(x,y,g.key_width,g.key_height,10,TFT_BLACK);
            if (focus_ == i) d.drawRoundRect(x+3,y+3,g.key_width-6,g.key_height-6,8,TFT_BLACK);
            if (i == (scientific_ ? 17 : 1)) {
                draw_icon_1bpp(d,calculator_icons::delete_icon,x+g.key_width/2,y+g.key_height/2);
                continue;
            }
            std::string label = key_label(i);
            system_font::draw_center(d,label,x+g.key_width/2,y+(g.key_height-font_h)/2);
        }
    }
    void draw_history(const Geometry &g) {
        auto &d = M5.Display;
        const int rows = history_rows();
        if (history.empty()) system_font::draw_center(d,tr(TextId::CalcHistoryEmpty),g.width/2,CONTENT_TOP+40);
        for (int row=0; row<rows; ++row) {
            const int index = static_cast<int>(history.size())-1-history_page_*rows-row;
            if (index < 0) break;
            const int y = CONTENT_TOP+row*132;
            system_font::draw_right(d,tail(pretty(history[index].expression),g.width-2*PAD),g.width-PAD,y+14);
            system_font::draw_right(d,"= " + history[index].result,g.width-PAD,y+62);
            d.drawFastHLine(PAD,y+131,g.width-2*PAD,TFT_BLACK);
        }
        system_font::draw_center(d,std::to_string(history_page_+1)+" / "+std::to_string(history_pages()),
                                 g.width/2,g.height-60);
    }
    void draw() {
        auto &d = M5.Display;
        const auto g = geometry();
        const bool quality = full_ || partial_count_ >= FULL_REFRESH_INTERVAL;
        const bool whole = full_ || quality || history_view_;
        d.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
        d.startWrite(); d.setDither(false);
        if (whole) {
            d.fillScreen(TFT_WHITE);
            draw_header(tr(history_view_ ? TextId::CalcHistory : TextId::Calculator));
            draw_actions(g.width);
            if (history_view_) draw_history(g);
            else { draw_result(g); draw_keyboard(g); }
            d.display();
        } else {
            draw_result(g);
            d.display(0,CONTENT_TOP,g.width,g.keypad_y-CONTENT_TOP);
        }
        d.endWrite();
        if (quality) partial_count_ = 0; else ++partial_count_;
        dirty_ = full_ = false;
    }
public:
    void on_create() override {
        core_ok_ = calculator::self_test();
        ESP_LOGI("CALCULATOR", "core self-test: %s", core_ok_ ? "passed" : "FAILED");
    }
    void on_show() override { dirty_ = full_ = true; }
    void loop() override {
        if (M5.Display.displayBusy()) return;
        if (exiting_) { ActivityManager::instance().pop(); return; }
        if (dirty_) draw();
        else StatusBarActivity::loop();
    }
    bool on_key(int key) override {
        if (exiting_) return true;
        if (key == APP_KEY_BACK) { back(); return true; }
        if (history_view_) {
            if (key == APP_KEY_UP || key == APP_KEY_DOWN) page(key == APP_KEY_UP ? -1 : 1);
        } else if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            focus_ = focus_ < 0 ? (key == APP_KEY_UP ? key_count()-1 : 0) :
                     (focus_+key_count()+(key == APP_KEY_UP ? -1 : 1))%key_count();
            dirty_ = full_ = true;
        } else if (key == APP_KEY_EXTRA) toggle_mode();
        else if (key == APP_KEY_OK) activate(focus_ < 0 ? key_count()-1 : focus_);
        return true;
    }
    bool on_touch(const TouchEvent &event) override {
        if (exiting_) return true;
        if (event.type == TouchEventType::Back) { back(); return true; }
        if (history_view_ && (event.type == TouchEventType::SwipeUp || event.type == TouchEventType::SwipeDown)) {
            page(event.type == TouchEventType::SwipeUp ? 1 : -1); return true;
        }
        if (event.type != TouchEventType::Tap) return true;
        if (status_bar_touch(event)) return true;
        if (calculator::layout::action(M5.Display.width(),false).contains(event.x,event.y)) {
            if (history_view_) back();
            else toggle_mode();
            return true;
        }
        if (!history_view_ && calculator::layout::action(M5.Display.width(),true).contains(event.x,event.y)) {
            history_view_ = true; history_page_ = 0; dirty_ = full_ = true; return true;
        }
        if (history_view_) return true;
        const auto g = geometry();
        if (event.x < PAD || event.y < g.keypad_y) return true;
        const int col = (event.x-PAD)/(g.key_width+GAP), row = (event.y-g.keypad_y)/(g.key_height+GAP);
        if (col < 4 && row < (scientific_ ? 9 : 5) && (event.x-PAD)%(g.key_width+GAP) < g.key_width &&
            (event.y-g.keypad_y)%(g.key_height+GAP) < g.key_height) activate(row*4+col);
        return true;
    }
};
}
void launch_calculator() { ActivityManager::instance().push(new CalculatorActivity()); }
