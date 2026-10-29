#pragma once
#include <algorithm>

namespace app_status_bar_layout {
constexpr int height=72, center_y=40, pixels=28;
struct Rect {
    int x,y,w,h;
    bool contains(int px,int py)const{return px>=x && px<x+w && py>=y && py<y+h;}
};
inline Rect title(int width,int clock_right) {
    const int center=width/2;
    const int half=std::max(0,std::min(center-clock_right-16,width-144-center));
    return {center-half,0,2*half,height};
}
inline Rect audio_output(int width) {return {width-134,height+12,110,56};}
inline bool back_hit(int x,int y) {return Rect{0,height,100,158-height}.contains(x,y);}
}
