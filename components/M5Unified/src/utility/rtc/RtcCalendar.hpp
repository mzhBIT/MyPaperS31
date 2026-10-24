#pragma once
#include <cstdint>

namespace m5::rtc_calendar {
constexpr bool valid(int year, int month, int day, int hour, int minute, int second) {
    constexpr int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 2000 || year > 2099 || month < 1 || month > 12) return false;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day >= 1 && day <= days[month-1] + (month == 2 && leap)
        && hour >= 0 && hour < 24 && minute >= 0 && minute < 60
        && second >= 0 && second < 60;
}
constexpr int64_t unix_seconds(int year, unsigned month, unsigned day,
                               int hour, int minute, int second) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = unsigned(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month-3 : month+9) + 2) / 5 + day-1;
    const unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return (era*146097LL + doe - 719468) * 86400 + hour*3600 + minute*60 + second;
}
constexpr int decode_bcd(uint8_t value) {
    return (value & 15) > 9 || (value >> 4) > 9 ? -1 : (value >> 4)*10 + (value & 15);
}
} // namespace m5::rtc_calendar
