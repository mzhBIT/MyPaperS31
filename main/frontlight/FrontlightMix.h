#pragma once
#include <cstdint>
namespace frontlight_mix {
constexpr int clamp(int value) { return value < 0 ? 0 : value > 10 ? 10 : value; }
struct Duty { uint32_t cool; uint32_t warm; };
constexpr Duty mix(int level, int temperature) {
    // Match P4: ten steps of 5%, with a combined warm+cool ceiling of 50%.
    const uint32_t total = (uint32_t(clamp(level)) * 1023 + 10) / 20;
    const uint32_t cool = (total * uint32_t(clamp(temperature)) + 5) / 10;
    return {cool, total - cool};
}
} // namespace frontlight_mix
