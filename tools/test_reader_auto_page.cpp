#include "ui/ReaderAutoPage.h"
#include "ui/reader_style.h"
#include <cassert>
#include <cstdio>

int main()
{
    static_assert(reader_auto_page_clamp_seconds(-5) == 0);
    static_assert(reader_auto_page_clamp_seconds(5) == 5);
    static_assert(reader_auto_page_clamp_seconds(299) == 295);
    static_assert(reader_auto_page_clamp_seconds(300) == 300);
    static_assert(reader_auto_page_clamp_seconds(305) == 300);
    static_assert(reader_auto_page_clamp_seconds(3600) == 300);
    ReaderAutoPage timer;
    assert(!timer.due(0, 5, true));
    assert(!timer.due(4999999, 5, true));
    assert(timer.due(5000000, 5, true));
    assert(!timer.due(5000000, 5, true));
    // Never catch up multiple pages after a stall.
    assert(timer.due(100000000, 5, true));
    assert(!timer.due(100000001, 5, true));
    // Busy / modal UI pauses and restarting waits a whole interval.
    assert(!timer.due(110000000, 5, false));
    assert(!timer.due(120000000, 5, true));
    assert(timer.due(125000000, 5, true));
    assert(!timer.due(140000000, 10, true));
    assert(!timer.due(149999999, 10, true));
    assert(timer.due(150000000, 10, true));
    timer.reset();
    assert(!timer.due(160000000, 10, true));
    assert(!timer.due(180000000, 0, true));
    assert(!timer.due(190000000, 5, true));
    assert(timer.due(195000000, 5, true));
    std::puts("Reader auto page timer tests passed");
}
