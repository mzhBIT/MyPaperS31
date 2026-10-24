#pragma once
#include <algorithm>
#include <cstdint>

namespace weread::reading_time {
// Use monotonic microseconds. Entry acknowledgements and disconnected gaps
// must never be backdated into an earlier active interval.
inline int64_t live_interval(int64_t previous,int64_t now,int64_t ready_since) {
    return ready_since>0 ? std::max<int64_t>(0,now-std::max(previous,ready_since)) : 0;
}
struct Slice { uint64_t total=0; uint32_t live=0; };
inline Slice due(uint64_t active_us,uint64_t live_us,uint64_t saved_total,
                 uint64_t consumed_live,bool context_ready) {
    Slice result;
    const uint64_t total=active_us/1000000, eligible=live_us/1000000;
    result.total=total>=saved_total?std::min<uint64_t>(total-saved_total,UINT32_MAX):0;
    if(context_ready&&eligible>=consumed_live)
        result.live=uint32_t(std::min(result.total,eligible-consumed_live));
    return result;
}
}
