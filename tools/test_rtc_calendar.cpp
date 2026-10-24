#include "utility/rtc/RtcCalendar.hpp"
using namespace m5::rtc_calendar;
static_assert(valid(2000,2,29,23,59,59));
static_assert(valid(2024,2,29,0,0,0));
static_assert(!valid(2025,2,29,0,0,0));
static_assert(!valid(2026,4,31,0,0,0));
static_assert(!valid(2100,1,1,0,0,0));
static_assert(!valid(2026,0,1,0,0,0));
static_assert(!valid(2026,1,1,24,0,0));
static_assert(!valid(2026,1,1,0,60,0));
static_assert(decode_bcd(0x59) == 59 && decode_bcd(0x1a) == -1);
static_assert(unix_seconds(2000,1,1,0,0,0) == 946684800);
static_assert(unix_seconds(2026,1,1,8,0,0) - 8*3600 == 1767225600);
static_assert(unix_seconds(2024,3,1,0,0,0) - unix_seconds(2024,2,28,0,0,0) == 2*86400);
int main() {}
