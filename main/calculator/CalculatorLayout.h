#pragma once
#include "ui/AppStatusBarLayout.h"
#include <algorithm>

namespace calculator::layout {
constexpr int padding=16, gap=12, toolbar_height=64;
constexpr int content_top=app_status_bar_layout::height+toolbar_height;
struct Geometry { int width,height,keypad_y,key_width,key_height; };
inline Geometry geometry(int width,int height,bool scientific) {
    const int result_height=scientific ? 180 : std::max(170,(height-content_top)/4);
    const int top=content_top+result_height, rows=scientific ? 9 : 5;
    return {width,height,top,(width-2*padding-3*gap)/4,
            (height-top-padding-(rows-1)*gap)/rows};
}
inline app_status_bar_layout::Rect action(int width,bool right) {
    const int button_width=(width-3*padding)/2;
    return {right ? width-padding-button_width : padding,
            app_status_bar_layout::height+8,button_width,toolbar_height-16};
}
}
