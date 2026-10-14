#include "activities/ReadingDays.h"
#include "ui/ReadingHomeLayout.h"
#include <cassert>
#include <cstdio>
#include <numeric>
int main() {
    using namespace reading_days;
    History a;
    constexpr int64_t day=20000;
    // Beijing 23:59:30 to 00:00:30: split equally, not all into exit day.
    a.add_span(day*day_us+(16*3600-30)*1000000LL,60*1000000ULL,480);
    assert(a.last_day==day+1);
    auto week=a.week(day+1);
    assert(week[5]==30 && week[6]==30);
    assert(a.week(day)[6]==30);
    History b; b.add(day+1,90*1000000ULL); a.merge(b);
    assert(a.week(day+1)[6]==120);
    // Repeated short pauses keep sub-second precision until display conversion.
    History short_intervals;
    for(int i=0;i<10;++i) short_intervals.add(day,125000);
    assert(short_intervals.week(day)[6]==1);
    a.add(day+40,1000000);
    assert(a.week(day+40)[6]==1);
    assert(a.week(day+40)[0]==0);
    a.add(day,1000000); assert(a.week(day+40)[0]==0);
    History invalid; invalid.add_span(-day_us,5000000,0); assert(invalid.last_day==-1);
    History west; west.add_span(day*day_us,3600000000ULL,-480);
    assert(west.week(day-1)[6]==3600);
    namespace ui=reading_home_layout;
    assert(ui::chart_hours(0)==3);
    assert(ui::chart_hours(3*3600+1)==6);
    assert(ui::bar_height(0,3)==0);
    assert(ui::bar_height(1,3)==5);
    assert(ui::bar_height(10800,3)==156);
    assert(ui::resume.contains(36,216));
    assert(!ui::resume.contains(36,619));
    const auto normal=ui::statistics(32,26);
    assert(normal.heading_y==643 && normal.card.y==698 && normal.grid_top==791);
    for(int heading : {32,48,66,80}) for(int label : {26,48,66}) {
        const auto g=ui::statistics(heading,label);
        assert(g.heading_y-(heading+1)/2>=625);
        assert(g.card.y>=g.heading_y+(heading+1)/2+24);
        assert(g.grid_top>=g.header_y+(label+1)/2+22);
        assert(g.grid_top<947 && g.card.y+g.card.h==1032);
        assert(ui::bar_height(10800,3,947-g.grid_top)==947-g.grid_top);
    }
    for(int h:{35,50,66}) {
        const int bar=ui::progress_y(h),action=ui::continue_y(h,h+4);
        assert(bar>=449+(h+1)/2+8);
        assert(action-(h+5)/2>=bar+9+12);
        assert(action+(h+5)/2<619);
    }
    std::puts("Reading days / dashboard layout: PASS");
}
