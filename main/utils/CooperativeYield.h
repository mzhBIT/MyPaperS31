#pragma once

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

// Task WDT is 5 seconds. Yield after one second of uninterrupted CPU work,
// leaving a wide margin without paying the 10 ms vTaskDelay(1) tick cost in
// every small parser or renderer iteration.
class CooperativeYield {
public:
    static constexpr int64_t kDefaultIntervalUs = 1000LL * 1000;

    explicit CooperativeYield(int64_t interval_us = kDefaultIntervalUs)
        : _last_yield_us(esp_timer_get_time()), _interval_us(interval_us) {}

    bool ifDue()
    {
        const int64_t now = esp_timer_get_time();
        if (now - _last_yield_us < _interval_us) return false;
        vTaskDelay(1);
        _last_yield_us = esp_timer_get_time();
        return true;
    }

private:
    int64_t _last_yield_us;
    int64_t _interval_us;
};
