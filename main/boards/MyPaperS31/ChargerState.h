#pragma once
#include <cstdint>

// CX25601N public registers, checked against the reference board's cx25601n.cc.
// Bus-independent so the two independent firmwares can recover the same PMIC
// state without sharing their I2C driver, display or application tasks.
namespace mypaper::board::s31::charger {
constexpr uint8_t Control1 = 0x16, Control3 = 0x18;
constexpr uint8_t EnableCharge = 0x20, HighImpedance = 0x10, Watchdog = 0x03;
constexpr uint8_t EnableOtg = 0x40;
struct State {
    uint8_t control1 = 0, control3 = 0, status = 0;
};
template <typename Read, typename Write>
bool recover_for_boot(Read read, Write write, State &before, State &after) {
    // An MCU reset does not reset the charger. Drop retained Host boost first,
    // and verify it is off before enabling the input/charging path.
    if (!read(Control1, before.control1) || !read(Control3, before.control3) ||
        !read(0x1e, before.status))
        return false;
    if (before.control3 & EnableOtg) {
        if (!write(Control3, uint8_t(before.control3 & ~EnableOtg)))
            return false;
    }
    if (!read(Control3, after.control3) || (after.control3 & EnableOtg))
        return false;
    const uint8_t desired = uint8_t((before.control1 & ~(HighImpedance | Watchdog)) | EnableCharge);
    if (before.control1 != desired && !write(Control1, desired))
        return false;
    // Preserve voltage/current, temperature, termination, safety timer and all
    // other limits. Do not unlock private registers or reset the entire chip.
    return read(Control1, after.control1) && read(0x1e, after.status) &&
           (after.control1 & (EnableCharge | HighImpedance | Watchdog)) == EnableCharge;
}
inline unsigned voltage_mv(uint8_t low, uint8_t high) {
    return (((low >> 3) & 31u) | ((high & 15u) << 5)) * 10;
}
inline unsigned charge_ma(uint8_t low, uint8_t high) {
    return (((low >> 6) & 3u) | ((high & 15u) << 2)) * 80;
}
inline unsigned input_ma(uint8_t low, uint8_t high) {
    return (((low >> 4) & 15u) | ((high & 15u) << 4)) * 20;
}
} // namespace mypaper::board::s31::charger
