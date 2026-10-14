#pragma once
#include "ui/BookshelfLayout.h"
#include <algorithm>
#include <cstdint>

// Original app_dashboard.c portrait geometry, 2026 mindreset (Apache-2.0).
namespace reading_home_layout {
using Rect=bookshelf_layout::Rect;
constexpr Rect resume{36,216,612,403};
constexpr Rect cover{36,292,172,250};
constexpr Rect chart{36,698,612,334};
constexpr int nav_top=1096;
constexpr int title_pixels=40; // Original resume title bypasses system scaling.
inline int progress_y(int detail_height) { return std::max(470,449+(detail_height+1)/2+8); }
inline int continue_y(int detail_height,int action_height) {
    return std::max(518,progress_y(detail_height)+9+12+(action_height+1)/2);
}
inline uint64_t chart_hours(uint64_t maximum_seconds) {
    uint64_t hours=std::max<uint64_t>(3,(maximum_seconds+3599)/3600);
    return (hours+2)/3*3;
}
inline int bar_height(uint64_t seconds,uint64_t hours,int available=156) {
    if(!seconds) return 0;
    return int(std::max<uint64_t>(5,std::min<uint64_t>(available,seconds*available/(hours*3600))));
}
struct Statistics {
    int heading_y,header_y,grid_top;
    Rect card;
};
inline Statistics statistics(int heading_ink,int header_ink) {
    const int heading=std::max(643,621+4+(heading_ink+1)/2);
    const int top=std::max(698,heading+(heading_ink+1)/2+24);
    const int header=top+45;
    return {heading,header,std::max(791,header+(header_ink+1)/2+22),{36,top,612,1032-top}};
}
}
