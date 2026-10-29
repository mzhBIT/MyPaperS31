#include "ui/AppStatusBarLayout.h"
#include <cassert>
#include <cstdio>

int main() {
    namespace l=app_status_bar_layout;
    for(int width:{684,1216})for(int clock_right:{100,180,260,width/2,width}) {
        const auto r=l::title(width,clock_right);
        assert(r.w>=0 && r.x+r.w/2==width/2 && r.y==0 && r.h==72);
        if(r.w) {assert(r.x>=clock_right+16);assert(r.x+r.w<=width-144);}
        const auto output=l::audio_output(width);
        assert(output.y>=l::height && output.x>=0 && output.x+output.w<=width);
        assert(output.contains(output.x,output.y));
        assert(!output.contains(output.x+output.w,output.y));
        assert(!output.contains(output.x,output.y+output.h));
        assert(!output.contains(output.x,40));
        assert(!l::back_hit(36,40));
        assert(l::back_hit(36,72));
        assert(!l::back_hit(100,72));
        assert(!l::back_hit(36,158));
    }
    std::puts("App status strip: title collision bounds, shared height, audio/back hit regions PASS");
}
