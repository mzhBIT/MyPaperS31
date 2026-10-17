#include "ui/BookshelfPress.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using bookshelf_layout::Style;

int main() {
    for(auto style:{Style::Acrylic,Style::DarkRail}) {
        bookshelf_press::Tracker tracker;
        TouchContact c;
        assert(tracker.update(c,style,0,9)==-1);
        for(int slot=0;slot<9;++slot) {
            const auto card=bookshelf_layout::card(style,slot);
            c={true,false,uint32_t(slot+1),card.x+90,card.y+90,card.x+90,card.y+90,0};
            assert(tracker.update(c,style,0,9)==slot);
            c.max_move=20;assert(tracker.update(c,style,0,9)==slot);
            c.max_move=21;assert(tracker.update(c,style,0,9)==-1);
            c.max_move=0;assert(tracker.update(c,style,0,9)==-1); // No re-arm on move back.
            c.down=false;assert(tracker.update(c,style,0,9)==-1);

            const auto area=bookshelf_press::row_area(slot),dirty=bookshelf_press::dirty_area(slot);
            const auto cover=bookshelf_layout::cover(style,slot),rail=bookshelf_layout::rail(slot/3);
            assert(dirty.x>=0 && dirty.x+dirty.w<=684 && dirty.x%2==0 && dirty.w%2==0);
            assert(dirty.w<=bookshelf_press::max_dirty_width);
            assert(area.y>=197 && area.y+area.h<=1032);
            assert(dirty.y==cover.y-16 && dirty.y+dirty.h>=rail.y+rail.h);
            assert(dirty.x<=cover.x-6 && dirty.x+dirty.w>=cover.x+cover.w+6);
            assert(area.y==dirty.y && area.h==dirty.h);
            assert(area.y==bookshelf_press::row_area(slot/3*3).y);
        }
        c={true,false,100,100,250,100,250,0};
        assert(tracker.update(c,style,0,1)==0);
        c.gesture_claimed=true;assert(tracker.update(c,style,0,1)==-1); // Long press / early swipe.
        c.gesture_claimed=false;assert(tracker.update(c,style,0,1)==-1);
        c.sequence++;assert(tracker.update(c,style,0,1)==0);
        tracker.cancel();assert(tracker.update(c,style,0,1)==-1);
        c.sequence++;c.x=400;assert(tracker.update(c,style,0,1)==-1); // Release over another card.
        c.sequence++;c.start_x=c.x=300;assert(tracker.update(c,style,0,1)==-1); // Empty cell.
        c.sequence++;c.start_x=c.x=100;assert(tracker.update(c,style,9,9)==-1); // Empty page.
        c.sequence++;assert(tracker.update(c,Style::List,0,9)==-1);
        c.sequence++;c.start_y=c.y=120;assert(tracker.update(c,style,0,9)==-1); // Header.
    }
    // Panel gray4 -> RGB565 -> gray4 snapshot conversion must not change grays.
    for(unsigned nibble=0;nibble<16;++nibble) {
        const unsigned red5=(nibble*17)>>3;
        assert((red5*15+15)/31==nibble);
    }
    std::puts("Bookshelf press: both grid styles, 9 slots, release/cancel/long/swipe, stable hit targets, dirty bounds and gray snapshots PASS");
}
