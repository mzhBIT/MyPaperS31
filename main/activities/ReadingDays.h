#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

// Calendar attribution uses wall time; elapsed duration always comes from the
// monotonic reading timer. No file IO occurs in this data structure.
namespace reading_days {
constexpr int count=30;
constexpr int64_t day_us=86400LL*1000000;
struct History {
    int64_t last_day=-1;
    std::array<uint64_t,count> us{};
    void add(int64_t day,uint64_t elapsed) {
        if(day<0 || !elapsed) return;
        if(last_day<0) last_day=day;
        if(day>last_day) {
            const int shift=int(std::min<int64_t>(count,day-last_day));
            for(int i=0;i<count-shift;++i) us[i]=us[i+shift];
            for(int i=count-shift;i<count;++i) us[i]=0;
            last_day=day;
        }
        const int64_t index=day-last_day+count-1;
        if(index<0 || index>=count) return;
        auto &value=us[size_t(index)];
        value+=std::min(elapsed,std::numeric_limits<uint64_t>::max()-value);
    }
    void add_span(int64_t utc_us,uint64_t elapsed,int offset_minutes) {
        int64_t local=utc_us+int64_t(offset_minutes)*60*1000000;
        if(local<0) return;
        // A single session can retain only the latest 30 days.
        const uint64_t retained=uint64_t(count)*day_us;
        if(elapsed>retained) { local+=int64_t(elapsed-retained); elapsed=retained; }
        while(elapsed) {
            const uint64_t part=std::min<uint64_t>(elapsed,day_us-local%day_us);
            add(local/day_us,part);
            local+=int64_t(part); elapsed-=part;
        }
    }
    void merge(const History &other) {
        if(other.last_day<0) return;
        for(int i=0;i<count;++i) add(other.last_day-count+1+i,other.us[i]);
    }
    std::array<uint64_t,7> week(int64_t today) const {
        std::array<uint64_t,7> result{};
        for(int i=0;i<7;++i) {
            const int64_t index=today-6+i-last_day+count-1;
            if(last_day>=0 && index>=0 && index<count) result[i]=us[size_t(index)]/1000000;
        }
        return result;
    }
};
}
