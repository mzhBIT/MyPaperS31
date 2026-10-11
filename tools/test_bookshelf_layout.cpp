#include "ui/BookshelfLayout.h"
#include "ui/pico_shelf_icons.h"

namespace b = bookshelf_layout;
constexpr bool valid(b::Rect r) {
    return r.x>=0 && r.y>=0 && r.w>0 && r.h>0 && r.x+r.w<=b::width && r.y+r.h<=b::height;
}
constexpr bool check(b::Style s) {
    for(int i=0;i<b::capacity(s);++i) {
        auto r=b::card(s,i), c=b::cover(s,i);
        if(!valid(r) || !valid(c) || !r.contains(c.x,c.y) ||
           !r.contains(c.x+c.w-1,c.y+c.h-1)) return false;
        if(r.contains(r.x+r.w,r.y) || r.contains(r.x,r.y+r.h)) return false;
        for(int j=0;j<b::capacity(s);++j)
            if((j==i)!=b::card(s,j).contains(c.x+c.w/2,c.y+c.h/2)) return false;
    }
    return true;
}
static_assert(check(b::Style::Acrylic));
static_assert(check(b::Style::DarkRail));
static_assert(check(b::Style::List));
static_assert(b::capacity(b::Style::List)==4);
static_assert(b::capacity(b::Style::Acrylic)==9);
static_assert(b::cover(b::Style::Acrylic,8).x==464);
static_assert(b::cover(b::Style::Acrylic,8).y==788);
static_assert(b::cover(b::Style::List,3).y==827);
static_assert(b::rail(2).y+b::rail(2).h==1014);
static_assert(!b::previous.contains(342,1053) && !b::next.contains(342,1053));
static_assert(sizeof(pico_shelf_icons::house)==2592);
static_assert(sizeof(pico_shelf_icons::library_big)==2592);
static_assert(sizeof(pico_shelf_icons::folder_open)==2592);
static_assert(sizeof(pico_shelf_icons::sliders_horizontal)==2592);
static_assert(b::ui_font_pixels(17,24)==19);
static_assert(b::ui_font_pixels(17,28)==22);
static_assert(b::ui_font_pixels(20,28)==26);
static_assert(b::ui_font_pixels(28,28)==35);
static_assert(b::ui_font_pixels(17,48)==38);
static_assert(b::ui_font_pixels(43,48)==90);
static_assert(b::ui_font_pixels(44,48)==46);
static_assert(b::ui_font_pixels(52,24)==54);
static_assert(b::ui_font_pixels(52,48)==54);
int main() { return 0; }
