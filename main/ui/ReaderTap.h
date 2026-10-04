#pragma once

struct ReaderTapAction {
    int page=0; // -1 previous, +1 next, 0 menu.
    int animation=0; // Same eight-direction codes as ReaderSwipe.
};

// Full-screen hit regions, independent of header visibility. Side columns
// have equal thirds; only the middle column has enlarged menu space.
inline ReaderTapAction reader_tap_action(int x,int y,int width,int height) {
    if (width<=0 || height<=0 || x<0 || y<0 || x>=width || y>=height) return {};
    const int side=width/3<200 ? width/3 : 200;
    const int side_top=height/3,side_bottom=height-height/3;
    if (x<side) {
        return {-1,y<side_top ? -3 : y>=side_bottom ? 4 : -1};
    }
    if (x>=width-side) {
        return {1,y<side_top ? -4 : y>=side_bottom ? 3 : 1};
    }
    const int cap=height/3<250 ? height/3 : 250;
    if (y<cap) return {-1,-2};
    if (y>=height-cap) return {1,2};
    return {};
}
