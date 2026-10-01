#pragma once
#include <cstdint>
#include <esp_err.h>

namespace mypaper::board {
enum class BoardId : uint8_t { MyPaperP4, MyPaperS31 };
enum class Capability : uint32_t {
    Display = 1u << 0, Touch = 1u << 1, SdCard = 1u << 2,
    Wifi = 1u << 3, Speaker = 1u << 4, Frontlight = 1u << 5,
    UsbMsc = 1u << 6, UsbAudio = 1u << 7, Modem = 1u << 8,
    Rtc = 1u << 9, BatteryGauge = 1u << 10,
    FrontlightCct = 1u << 11, Vibration = 1u << 12,
};
constexpr uint32_t bit(Capability capability) { return static_cast<uint32_t>(capability); }
struct Profile {
    BoardId id;
    const char* name;
    const char* target;
    const char* touch_controller;
    const char* audio_backend;
    uint16_t panel_width;
    uint16_t panel_height;
    uint8_t epd_bus_width;
    // Compiled support, NOT initialization success or device online.
    uint32_t driver_capabilities;
    bool bootstrap_only;
};
const Profile& profile();
struct HardwareInitResult {
    esp_err_t sd_status;
    bool display_ready;
};
HardwareInitResult initialize_hardware(bool usb_transfer_mode);
bool supports(Capability capability);
void log_profile();
} // namespace mypaper::board
