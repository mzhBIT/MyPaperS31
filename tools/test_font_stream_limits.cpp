#include "renderer/FontStreamLimits.h"
#include <cassert>
#include <cstdint>
#include <limits>
#include <cstdio>

int main() {
    using font_stream_limits::supported;
    constexpr auto seek_max=std::numeric_limits<int32_t>::max();
    constexpr auto ft_max=std::numeric_limits<int32_t>::max();
    constexpr auto stream_max=std::numeric_limits<uint32_t>::max();
    assert(supported(35027008,seek_max,ft_max,stream_max));
    assert(supported(32*1024*1024+1,seek_max,ft_max,stream_max));
    assert(supported(128*1024*1024,seek_max,ft_max,stream_max));
    assert(!supported(0,seek_max,ft_max,stream_max));
    assert(!supported(-1,seek_max,ft_max,stream_max));
    assert(supported(seek_max,seek_max,ft_max,stream_max));
    assert(!supported(int64_t(seek_max)+1,seek_max,ft_max,stream_max));
    assert(!supported(100,99,ft_max,stream_max));
    assert(!supported(100,seek_max,99,stream_max));
    assert(!supported(100,seek_max,ft_max,99));
    std::puts("streamed font file limits: 33.4 MiB regression and offset bounds PASS");
}
