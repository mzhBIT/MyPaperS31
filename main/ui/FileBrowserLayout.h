#pragma once

// Portrait geometry adapted from read-pico-reader app_files.c.
// Copyright 2026 mindreset. SPDX-License-Identifier: Apache-2.0
namespace file_browser_layout {
struct Rect {
    int x, y, w, h;
    constexpr bool contains(int px, int py) const {
        return px >= x && px < x+w && py >= y && py < y+h;
    }
};
constexpr int nav_top = 1096, row_height = 91;
constexpr Rect storage{36,171,612,112};
constexpr Rect transfer_add{592,90,56,56};
constexpr Rect transfer_menu{248,158,400,332};
constexpr Rect transfer(int i) { return {268,178+i*100,360,92}; }
constexpr Rect sheet{20,430,644,626};
constexpr Rect close{596,446,56,64};
constexpr Rect cancel{40,930,604,76};
constexpr Rect action(int i) { return {40+(i%2)*312,648+(i/2)*122,292,100}; }
constexpr int rows(bool home) { return home ? 7 : 9; }
constexpr int list_top(bool home) { return home ? 362 : 214; }
constexpr int row_at(bool home, int y) {
    return y >= list_top(home) && y < list_top(home)+rows(home)*row_height
        ? (y-list_top(home))/row_height : -1;
}
// Font metrics determine content bounds; rendering and hit testing share them.
struct Content {
    int path_y, separator_y, top, row_h, count;
    Rect storage_card, sort_button;
    int storage_usage_y, root_y, root_h;
    constexpr int bottom() const { return top + count * row_h; }
    constexpr int row_at(int y) const {
        return y >= top && y < bottom() ? (y-top)/row_h : -1;
    }
};
constexpr int maximum(int a,int b) { return a>b?a:b; }
constexpr Content content(bool home,int title_h,int path_h,int name_h,int detail_h,
                          int root_h=0,int storage_title_h=0,int sort_w=111) {
    const int path_y=maximum(172,107+(title_h+1)/2+12);
    const int separator=path_y+path_h+12;
    const int storage_first_h=maximum(storage_title_h,detail_h);
    const int usage_y=storage.y+16+storage_first_h+8;
    const int storage_h=maximum(storage.h,16+storage_first_h+8+detail_h+16);
    const int root_y=storage.y+storage_h+16;
    const int root_height=maximum(50,maximum(root_h,detail_h)+16);
    const int top=home?maximum(362,root_y+root_height+12):maximum(214,separator+12);
    const int height=maximum(91,name_h+detail_h+32);
    const int limit=1025;
    return {path_y,separator,top,height,maximum(1,(limit-top)/height),
        {storage.x,storage.y,storage.w,storage_h},{648-sort_w,root_y,sort_w,root_height},
        usage_y,root_y,root_height};
}
}
