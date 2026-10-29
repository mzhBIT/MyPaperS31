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
    for (int temperature = 0; temperature <= 10; ++temperature) {
        unsigned previous = 0;
        for (int value = 0; value <= 1000; ++value) {
            const auto d = mix_fine(value, temperature);
            assert(d.cool + d.warm >= previous && d.cool + d.warm <= 512);
            previous = d.cool + d.warm;
            if (value % 100 == 0) {
                const auto legacy = mix(value / 100, temperature);
                assert(legacy.cool == d.cool && legacy.warm == d.warm);
            }
        }
    }
    for (int brightness = 0; brightness <= 1000; ++brightness) {
        const unsigned total = mix_continuous(brightness, 0).warm;
        unsigned previous_cool = 0;
        for (int cct = 0; cct <= 1000; ++cct) {
            const auto d = mix_continuous(brightness, cct);
            assert(d.cool + d.warm == total && total <= 512);
            assert(d.cool >= previous_cool);
            previous_cool = d.cool;
            if (cct % 100 == 0) {
                const auto legacy = mix_fine(brightness, cct / 100);
                assert(d.cool == legacy.cool && d.warm == legacy.warm);
            }
        }
    }
    const auto a = mix_continuous(1000, 501), b = mix_continuous(1000, 510);
    assert(a.cool != b.cool); // Fine changes no longer round to eleven CCT steps.
    assert(mix_continuous(-1, 500).cool == 0);
    assert(mix_continuous(1001, -1).warm == 512);
    assert(mix_continuous(1001, 1001).cool == 512);
    std::puts("S31 frontlight blend: off/endpoints/clamping/monotonicity/constant total duty PASS");
}
