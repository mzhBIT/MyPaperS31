#include "frontlight/FrontlightMix.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace frontlight_mix;
    for (int level = -2; level <= 12; ++level) {
        const auto warm = mix(level, 0), cool = mix(level, 10);
        assert(warm.cool == 0 && cool.warm == 0);
        assert(warm.warm == cool.cool && cool.cool <= 512);
        assert(warm.warm == (uint32_t(clamp(level)) * 1023 + 10) / 20);
        uint32_t previous_cool = 0;
        for (int temperature = -2; temperature <= 12; ++temperature) {
            const auto d = mix(level, temperature);
            assert(d.cool + d.warm == warm.warm);
            assert(d.cool <= 512 && d.warm <= 512);
            assert(d.cool >= previous_cool);
            if (level <= 0) assert(d.cool == 0 && d.warm == 0);
            previous_cool = d.cool;
        }
        const auto middle = mix(level, 5);
        assert(middle.cool == middle.warm || middle.cool == middle.warm + 1);
    }
    std::puts("S31 frontlight blend: off/endpoints/clamping/monotonicity/constant total duty PASS");
}
