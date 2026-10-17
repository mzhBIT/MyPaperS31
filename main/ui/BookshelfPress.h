#pragma once
#include "ui/BookshelfLayout.h"
#include "input/touch_input.h"

namespace bookshelf_press {
constexpr int lift = 16;
constexpr int cover_bytes = 164 * 214 / 2;
constexpr int row_height = 242; // 16 px above covers through the rail bottom.
constexpr int max_dirty_width = 182;

inline constexpr bookshelf_layout::Rect row_area(int slot) {
    return {0, bookshelf_layout::cover(bookshelf_layout::Style::Acrylic,slot).y-lift,
            684,row_height};
}
inline constexpr bookshelf_layout::Rect dirty_area(int slot) {
    const auto cover=bookshelf_layout::cover(bookshelf_layout::Style::Acrylic,slot);
    // Include the acrylic kernel's six-pixel horizontal halo; even 4bpp width.
    const int left=(cover.x-8)&~1,right=(cover.x+cover.w+9)&~1;
    return {left,cover.y-lift,right-left,row_height};
}

class Tracker {
    uint32_t sequence_ = 0;
    bool tracked_ = false, cancelled_ = true;
    int slot_ = -1;
public:
    void cancel() { cancelled_=true; slot_=-1; }
    int update(const TouchContact& contact, bookshelf_layout::Style style,
               int first, int count) {
        if (!contact.down) { cancel(); return -1; }
        if (!tracked_ || sequence_!=contact.sequence) {
            sequence_=contact.sequence; tracked_=true; cancelled_=false; slot_=-1;
            if (style!=bookshelf_layout::Style::List) {
                for(int slot=0;slot<9 && first+slot<count;++slot)
                    if(bookshelf_layout::card(style,slot).contains(contact.start_x,contact.start_y)) {
                        slot_=slot;break;
                    }
            }
        }
        if(slot_<0 || cancelled_) return -1;
        if(contact.gesture_claimed || contact.max_move>20 ||
           !bookshelf_layout::card(style,slot_).contains(contact.x,contact.y)) cancel();
        return cancelled_ ? -1 : slot_;
    }
};
}
