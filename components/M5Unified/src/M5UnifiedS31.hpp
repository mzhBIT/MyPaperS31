#pragma once
#include <M5GFX.h>
#include "utility/Button_Class.hpp"
#include "utility/power/BQ27220_Class.hpp"
#include "utility/rtc/PCF8563_Class.hpp"
#include <ctime>

namespace m5 {
// Minimal application facade, not an emulation of the P4 hardware. Display
// is initialized by BoardS31. PCF8563 uses the existing shared I2C bus.
class S31SystemClock {
public:
    bool begin();
    bool isEnabled() const { return rtc_.isEnabled(); }
    // Hardware calendar is fixed Beijing time; APIs use UTC Unix seconds.
    bool readUtc(time_t* utc) const;
    bool writeUtc(time_t utc);
    rtc_datetime_t getDateTime() const {
        rtc_datetime_t result;
        const time_t now = std::time(nullptr);
        tm value = {};
        if (localtime_r(&now, &value)) {
            result = rtc_datetime_t(value);
        }
        return result;
    }
private:
    PCF8563_Class rtc_{0x51, 100000};
};
class S31Power {
public:
    bool begin();
    int getBatteryLevel(); // -1 when no recent valid sample, otherwise 0..100
    int getBatteryVoltage(); // mV; 0 when unavailable
    int getBatteryCurrent(); // mA, positive charge / negative discharge
    bool isCharging();
    void update();
private:
    BQ27220_Class gauge_;
    bool initialized_ = false;
    uint32_t last_attempt_ = 0, last_valid_ = 0, last_step_ = 0, full_since_ = 0;
    uint32_t sample_interval_ = 1000;
    uint32_t last_log_ = 0;
    bool full_pending_ = false;
    uint16_t voltage_ = 0;
    int16_t current_ = 0;
    int level_ = -1, direction_ = 0;
    int samples_[60] = {}, sum_ = 0, sample_pos_ = 0;
};
class M5Unified {
public:
    m5gfx::M5GFX Display;
    m5gfx::M5GFX& Lcd = Display;
    S31SystemClock Rtc;
    S31Power Power;
};
}
extern m5::M5Unified M5;
