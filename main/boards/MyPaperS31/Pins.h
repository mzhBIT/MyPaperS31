#pragma once
#include <array>
#include "StoragePins.h"
namespace mypaper::board::s31 {
// Reference V1.4 wiring, pending real-board confirmation. Not driven at boot.
inline constexpr std::array<int, 8> epd_data{8, 9, 10, 11, 12, 13, 14, 15};
inline constexpr int epd_xcl = 16, epd_xle = 17, epd_xoe = 18;
inline constexpr int epd_xstl = 4, epd_mode = 19;
inline constexpr int epd_ckv = 40, epd_spv = 35, epd_border = 37;
inline constexpr int i2c_sda = 0, i2c_scl = 1, io_interrupt = 2;
inline constexpr int touch_interrupt = 5, touch_reset_io = 10;
inline constexpr int io_address = 0x20, touch_address = 0x38, tps_address = 0x68;
inline constexpr int tps_vcom_io = 0, tps_pwrup_io = 1, tps_wake_io = 2, tps_pg_io = 7;
inline constexpr int frontlight_cool = 6, frontlight_warm = 7;
inline constexpr int sd_clk = MYPAPER_S31_SD_CLK, sd_cmd = MYPAPER_S31_SD_CMD;
inline constexpr std::array<int, 4> sd_data{MYPAPER_S31_SD_D0, MYPAPER_S31_SD_D1,
                                            MYPAPER_S31_SD_D2, MYPAPER_S31_SD_D3};
inline constexpr int i2s_bclk = 42, i2s_ws = 43, i2s_dout = 44, i2s_din = 45;
inline constexpr int bt_tx = 39, bt_rx = 38, usb_analog_switch = 36;
inline constexpr int rtc_address = 0x51, gauge_address = 0x55, charger_address = 0x6b;
inline constexpr int boot_button = 61;
inline constexpr int power_key_io = 15, power_pulse_io = 11;
inline constexpr int volume_up_io = 8, volume_down_io = 9;
inline constexpr int rtc_interrupt_io = 14, motor_io = 3, pa_enable_io = 4;
inline constexpr int screen_power_io = 5, bt_power_io = 12;
} // namespace mypaper::board::s31
