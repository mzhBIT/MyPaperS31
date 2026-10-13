#include "../main/ui/FileBrowserLayout.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
namespace fl = file_browser_layout;
int main() {
    for(bool home : {true,false}) {
        const int top=fl::list_top(home), rows=fl::rows(home);
        assert(fl::row_at(home,top-1)==-1);
        assert(fl::row_at(home,top+rows*fl::row_height)==-1);
        assert(top+rows*fl::row_height < fl::nav_top);
        for(int i=0;i<rows;++i) {
            assert(fl::row_at(home,top+i*fl::row_height)==i);
            assert(fl::row_at(home,top+(i+1)*fl::row_height-1)==i);
        }
    }
    for(int i=0;i<3;++i) {
        auto r=fl::transfer(i);
        assert(r.contains(r.x,r.y));
        assert(!r.contains(r.x+r.w,r.y));
        assert(!r.contains(r.x,r.y+r.h));
        assert(r.x>=0 && r.x+r.w<=684);
        assert(fl::transfer_menu.contains(r.x,r.y));
        assert(fl::transfer_menu.contains(r.x+r.w-1,r.y+r.h-1));
        if(i<2) assert(r.y+r.h<=fl::transfer(i+1).y);
    }
    for(int i=0;i<3;++i) {
        auto r=fl::action(i);
        assert(fl::sheet.contains(r.x,r.y));
        assert(fl::sheet.contains(r.x+r.w-1,r.y+r.h-1));
        assert(r.y+r.h < fl::cancel.y);
    }
    assert(fl::cancel.y+fl::cancel.h < fl::sheet.y+fl::sheet.h);
    assert(fl::sheet.y+fl::sheet.h < fl::nav_top);
    for(bool home : {true,false}) {
        for(int name_h : {30,42,60,80}) {
            const auto c=fl::content(home,name_h+10,name_h,name_h,name_h-4,name_h,name_h+8,180);
            assert(c.row_h>=name_h+(name_h-4)+32);
            assert(c.count>=1);
            if(home) {
                assert(c.storage_usage_y+name_h-4+16<=c.storage_card.y+c.storage_card.h);
                assert(c.root_y>=c.storage_card.y+c.storage_card.h+16);
                assert(c.root_h>=name_h+16);
                assert(c.top>=c.root_y+c.root_h+12);
                assert(c.sort_button.y==c.root_y && c.sort_button.h==c.root_h);
                assert(c.sort_button.x+c.sort_button.w==648);
            }
            assert(c.bottom()<=1025);
            if(!home) {
                assert(c.separator_y>=c.path_y+name_h+12);
                assert(c.top>=c.separator_y+12);
            }
            assert(c.row_at(c.top-1)==-1);
            assert(c.row_at(c.bottom())==-1);
            for(int i=0;i<c.count;++i) {
                assert(c.row_at(c.top+i*c.row_h)==i);
                assert(c.row_at(c.top+(i+1)*c.row_h-1)==i);
            }
        }
    }
    const auto expanded=fl::content(true,50,34,42,34);
    const int previous=(975-520)/expanded.row_h;
    assert(expanded.count>previous);
    std::puts("File browser layout: PASS");
}
