#include "lgfx/v1/platforms/esp32/EpdOeGateTest.h"
using namespace lgfx_epd_oe_test;
constexpr bool check(unsigned ticks, unsigned first, unsigned end) {
    Pulse p[32]{};
    const auto n = build(p, 32, 684, ticks, first, end);
    if (!n || (n&1)) return false;
    unsigned total = 0, high = 0, changes = 0;
    for (size_t i=0; i<n; ++i) {
        if (!p[i].ticks || p[i].ticks > 32767 || p[i].level > 1) return false;
        total += p[i].ticks;
        if (p[i].level) high += p[i].ticks;
        if (i && p[i].level != p[i-1].level) ++changes;
    }
    const unsigned start = first > 8 ? (first-8+3)*ticks : 0;
    const unsigned finish = end+8 < 684 ? (end+8+3)*ticks : 689*ticks;
    return total >= 689*ticks && total <= 689*ticks+1
        && high == finish-start && changes <= 2;
}
static_assert(check(160,228,456));
static_assert(check(200,228,456));
static_assert(check(160,0,1));
static_assert(check(160,683,684));
static_assert(check(160,0,684));
constexpr bool reject() {
    Pulse p[32]{};
    return !build(p,32,684,160,5,5) && !build(p,32,684,160,0,685)
        && !build(p,0,684,160,228,456) && !build(p,32,684,0,228,456);
}
static_assert(reject());
int main() {}
