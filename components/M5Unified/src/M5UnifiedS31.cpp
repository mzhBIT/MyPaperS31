#include "M5UnifiedS31.hpp"
#include <esp_log.h>
#include <algorithm>
#include <iterator>
#include "utility/rtc/RtcCalendar.hpp"
#include <sys/time.h>

namespace m5 {
bool S31SystemClock::begin() {
    if (!In_I2C.begin(static_cast<i2c_port_t>(1), 0, 1) || !rtc_.begin()) {
        ESP_LOGW("S31_RTC", "PCF8563 @0x51 unavailable");
        return false;
    }
    time_t utc;
    if (!readUtc(&utc) || utc < 1704067200) {
        ESP_LOGW("S31_RTC", "PCF8563 ready, time invalid/VL; set manually or synchronize from network");
        return true;
    }
    timeval tv{utc, 0};
    const bool ok = settimeofday(&tv, nullptr) == 0;
    ESP_LOGI("S31_RTC", "boot RTC -> system: unix=%lld applied=%d storage=UTC+08:00",
             static_cast<long long>(utc), ok);
    return ok;
}
bool S31SystemClock::readUtc(time_t* utc) const {
    uint8_t raw[7];
    if (!utc || !isEnabled() || !rtc_.readRegister(0x02, raw, sizeof(raw))
        || (raw[0] & 0x80) || (raw[5] & 0x80)) return false;
    using namespace rtc_calendar;
    const int second = decode_bcd(raw[0] & 0x7f), minute = decode_bcd(raw[1] & 0x7f);
    const int hour = decode_bcd(raw[2] & 0x3f), day = decode_bcd(raw[3] & 0x3f);
    const int month = decode_bcd(raw[5] & 0x1f), yy = decode_bcd(raw[6]);
    if (yy < 0 || !valid(2000+yy, month, day, hour, minute, second)) return false;
    *utc = static_cast<time_t>(unix_seconds(2000+yy, month, day, hour, minute, second) - 8*3600);
    return true;
}
bool S31SystemClock::writeUtc(time_t utc) {
    if (!isEnabled()) return false;
    const time_t beijing = utc + 8*3600;
    tm local{};
    if (!gmtime_r(&beijing, &local) || !rtc_calendar::valid(local.tm_year+1900,
        local.tm_mon+1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec)) return false;
    const rtc_date_t date(local);
    const rtc_time_t time(local);
    if (!rtc_.setDateTime(&date, &time)) return false;
    time_t readback;
    const bool ok = readUtc(&readback) && readback >= utc && readback <= utc+2;
    ESP_LOGI("S31_RTC", "system -> RTC: unix=%lld verified=%d", static_cast<long long>(utc), ok);
    return ok;
}
bool S31Power::begin() {
    if (initialized_) return true;
    // Share LGFX's existing bus/lock; never install another IDF I2C driver.
    if (!In_I2C.begin(static_cast<i2c_port_t>(1), 0, 1)) return false;
    initialized_ = true;
    last_attempt_ = lgfx::millis();
    uint16_t mv = 0;
    const bool ok = gauge_.beginReadOnly(mv);
    if (ok) ESP_LOGI("S31_BATTERY", "BQ27220 @0x55 read-only ready: voltage=%u mV", mv);
    else ESP_LOGW("S31_BATTERY", "BQ27220 unavailable; battery remains unknown, retry on status read");
    return ok;
}
void S31Power::update() {
    if (!initialized_ || M5.Display.displayBusy()) return;
    const uint32_t now = lgfx::millis();
    // No polling task: idle service/status reads share one cached sample. Failed devices
    // retry at most every 30 seconds, not on each getter/frame.
    if (now - last_attempt_ < sample_interval_) return;
    last_attempt_ = now;
    uint16_t mv; int16_t ma;
    if (!gauge_.readMeasurement(mv, ma)) {
        full_pending_ = false;
        sample_interval_ = 30000;
        ESP_LOGW("S31_BATTERY", "read failed; retaining last sample for at most 120 s");
        return;
    }
    voltage_ = mv; current_ = ma; last_valid_ = now;
    const int raw = std::clamp((int(mv) - 3300) * 100 / 1030, 0, 100);
    if (level_ < 0) { std::fill(std::begin(samples_), std::end(samples_), raw); sum_ = raw * 60; }
    sum_ += raw - samples_[sample_pos_]; samples_[sample_pos_] = raw;
    sample_pos_ = (sample_pos_ + 1) % 60;
    int target = (sum_ + 30) / 60;
    if (mv >= 4330) { if (!full_pending_) full_since_ = now; full_pending_ = true; }
    else full_pending_ = false;
    const bool full = full_pending_ && now - full_since_ >= 30000;
    if (target == 100 && !full) target = 99;
    if (level_ == 100 && mv >= 4280) target = 100;
    const int dir = ma > 5 ? 1 : (ma < -5 ? -1 : 0);
    if (level_ < 0) { level_ = target; last_step_ = now; }
    else {
        if (dir != direction_) last_step_ = now;
        if (now - last_step_ >= 30000) {
            if (dir > 0 && target > level_) ++level_;
            else if (dir <= 0 && target < level_) --level_;
            last_step_ = now;
        }
        if (full && target == 100 && level_ >= 99) level_ = 100;
    }
    direction_ = dir; sample_interval_ = 1000;
    if (!last_log_ || now - last_log_ >= 30000) {
        last_log_ = now;
        ESP_LOGI("S31_BATTERY", "voltage=%u mV current=%d mA level=%d%% source=voltage-estimate", mv, ma, level_);
    }
}
int S31Power::getBatteryLevel() {
    update();
    return level_ >= 0 && lgfx::millis() - last_valid_ <= 120000 ? level_ : -1;
}
int S31Power::getBatteryVoltage() { return getBatteryLevel() >= 0 ? voltage_ : 0; }
int S31Power::getBatteryCurrent() { return getBatteryLevel() >= 0 ? current_ : 0; }
bool S31Power::isCharging() { return getBatteryLevel() >= 0 && current_ > 5 && level_ < 100; }
}
m5::M5Unified M5;
