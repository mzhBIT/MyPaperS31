#pragma once
#include "ui/AppStatusBarLayout.h"
#include "ui/ShelfStatusBar.h"
#include "input/touch_input.h"

// Home geometry and icon rendering, plus a centred, collision-safe app title.
namespace app_status_bar {
constexpr int height=app_status_bar_layout::height;
static_assert(height==shelf_status_bar::height);
inline std::string fit(std::string text,int width,int px) {
    if(width<=0)return {};
    if(system_font::measure(text,px)<=width)return text;
    const std::string suffix="...";
    if(system_font::measure(suffix,px)>width)return {};
    while(!text.empty() && system_font::measure(text+suffix,px)>width) {
        size_t at=text.size()-1;
        while(at && (static_cast<unsigned char>(text[at])&0xc0)==0x80)--at;
        text.resize(at);
    }
    return text+suffix;
}
class State {
    std::string _clock;
    uint32_t _last_draw=0,_last_check=0;
    bool _wifi=false;
public:
    void draw(const std::string& title) {
        M5.Display.fillRect(0,0,M5.Display.width(),height,TFT_WHITE);
        _clock=shelf_status_bar::draw();
        const auto box=app_status_bar_layout::title(M5.Display.width(),
            status_clock::left+system_font::measure(_clock,status_clock::pixels));
        const auto label=fit(title,box.w,app_status_bar_layout::pixels);
        system_font::draw_on_background(M5.Display,label,
            M5.Display.width()/2-system_font::measure(label,app_status_bar_layout::pixels)/2,
            system_font::centered_y(label,app_status_bar_layout::center_y,app_status_bar_layout::pixels),
            app_status_bar_layout::pixels,TFT_BLACK,TFT_WHITE);
        _wifi=wifi_setting_is_enabled();
        _last_draw=_last_check=lgfx::millis();
    }
    void refresh(const char* title) {
        const uint32_t now=lgfx::millis();
        if(uint32_t(now-_last_check)<1000 || M5.Display.displayBusy() || touch_input_needs_fast_poll())return;
        _last_check=now;
        if(uint32_t(now-_last_draw)<60000 && _clock==status_clock::text() &&
            _wifi==wifi_setting_is_enabled())return;
        const auto previous=M5.Display.getEpdMode();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw(title);
        M5.Display.display(0,0,M5.Display.width(),height);
        M5.Display.endWrite();
        M5.Display.setEpdMode(previous);
    }
};
inline void draw_audio_output(const char* label) {
    const auto r=app_status_bar_layout::audio_output(M5.Display.width());
    M5.Display.drawRect(r.x,r.y,r.w,r.h,TFT_BLACK);
    const auto text=fit(label,r.w-12,24);
    system_font::draw_on_background(M5.Display,text,r.x+(r.w-system_font::measure(text,24))/2,
        system_font::centered_y(text,r.y+r.h/2,24),24,TFT_BLACK,TFT_WHITE);
}
}
