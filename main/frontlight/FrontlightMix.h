#pragma once
#include <cstdint>
namespace frontlight_mix {
constexpr int clamp(int value) { return value < 0 ? 0 : value > 10 ? 10 : value; }
struct Duty { uint32_t cool; uint32_t warm; };
constexpr int clamp_fine(int value) { return value < 0 ? 0 : value > 1000 ? 1000 : value; }
constexpr Duty mix_continuous(int brightness, int temperature) {
    // Match P4's combined warm+cool PWM ceiling of 50%.
    const uint32_t total = (uint32_t(clamp_fine(brightness)) * 1023 + 1000) / 2000;
    const uint32_t cool = (total * uint32_t(clamp_fine(temperature)) + 500) / 1000;
    return {cool, total - cool};
}
constexpr Duty mix_fine(int brightness, int temperature) { return mix_continuous(brightness, clamp(temperature) * 100); }
constexpr Duty mix(int level, int temperature) { return mix_fine(clamp(level) * 100, temperature); }
} // namespace frontlight_mix
