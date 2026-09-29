#include "time/time_basis.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "nvs.h"

namespace {

constexpr char NVS_NAMESPACE[] = "time_basis";
constexpr char NVS_KEY[] = "offset_min";
constexpr char NVS_CLOCK_KEY[] = "clock_utc";
constexpr char NVS_HOUR_KEY[] = "hour_24";
constexpr int DEFAULT_OFFSET = 8 * 60;
std::atomic<int> s_offset_minutes{100000};
std::atomic<int> s_clock_is_utc{-1};
std::atomic<int> s_use_24_hour{-1};

bool leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month)
{
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && leap_year(year) ? 1 : 0);
}

bool valid_date(int year, int month, int day, int hour, int minute, int second)
{
    return year >= 1970 && year <= 2099 && month >= 1 && month <= 12 &&
           day >= 1 && day <= days_in_month(year, month) &&
           hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 &&
           second >= 0 && second <= 59;
}

// Civil-calendar conversion avoids relying on the ESP system clock or TZ state.
int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<int>(doe) - 719468;
}

void civil_from_days(int64_t days, int *year, unsigned *month, unsigned *day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = y + (*month <= 2);
}

int two_digits(const char *text, size_t at)
{
    if (text[at] < '0' || text[at] > '9' ||
        text[at + 1] < '0' || text[at + 1] > '9') return -1;
    return (text[at] - '0') * 10 + text[at + 1] - '0';
}

bool make_utc(int year, int month, int day, int hour, int minute, int second,
              int source_offset_minutes, int64_t *utc)
{
    if (!utc || !valid_date(year, month, day, hour, minute, second)) return false;
    *utc = days_from_civil(year, static_cast<unsigned>(month),
                           static_cast<unsigned>(day)) * 86400 +
           hour * 3600 + minute * 60 + second - source_offset_minutes * 60;
    return true;
}

bool parse_modem(const char *raw, bool clock_digits_are_utc, int64_t *utc_seconds)
{
    if (!raw || !utc_seconds) return false;
    const size_t length = std::strlen(raw);
    if (length != 17 && length != 20) return false;
    if (raw[2] != '/' || raw[5] != '/' || raw[8] != ',' || raw[11] != ':' ||
        raw[14] != ':') return false;
    const int yy = two_digits(raw, 0);
    const int month = two_digits(raw, 3);
    const int day = two_digits(raw, 6);
    const int hour = two_digits(raw, 9);
    const int minute = two_digits(raw, 12);
    const int second = two_digits(raw, 15);
    if (yy < 0) return false;
    int source_offset = clock_digits_are_utc ? 0 : DEFAULT_OFFSET;
    if (length == 20) {
        const int quarters = two_digits(raw, 18);
        if ((raw[17] != '+' && raw[17] != '-') || quarters < 0 || quarters > 96) {
            return false;
        }
        if (!clock_digits_are_utc) {
            source_offset = quarters * 15 * (raw[17] == '+' ? 1 : -1);
        }
    }
    return make_utc(yy >= 70 ? 1900 + yy : 2000 + yy, month, day,
                    hour, minute, second, source_offset, utc_seconds);
}

} // namespace

extern "C" int time_basis_get_offset_minutes(void)
{
    const int cached = s_offset_minutes.load();
    if (cached != 100000) return cached;
    int loaded = DEFAULT_OFFSET;
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        int16_t saved = DEFAULT_OFFSET;
        if (nvs_get_i16(handle, NVS_KEY, &saved) == ESP_OK &&
            saved >= -12 * 60 && saved <= 14 * 60 && saved % 30 == 0) {
            loaded = saved;
        }
        nvs_close(handle);
    }
    int expected = 100000;
    s_offset_minutes.compare_exchange_strong(expected, loaded);
    return s_offset_minutes.load();
}

extern "C" esp_err_t time_basis_set_offset_minutes(int minutes)
{
    if (minutes < -12 * 60 || minutes > 14 * 60 || minutes % 30 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_i16(handle, NVS_KEY, static_cast<int16_t>(minutes));
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret == ESP_OK) s_offset_minutes.store(minutes);
    return ret;
}

extern "C" bool time_basis_modem_clock_is_utc(void)
{
    const int cached = s_clock_is_utc.load();
    if (cached >= 0) return cached != 0;
    uint8_t loaded = 1; // ML307A's observed CCLK digits are UTC despite +32.
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, NVS_CLOCK_KEY, &loaded);
        nvs_close(handle);
    }
    int expected = -1;
    s_clock_is_utc.compare_exchange_strong(expected, loaded != 0 ? 1 : 0);
    return s_clock_is_utc.load() != 0;
}

extern "C" esp_err_t time_basis_set_modem_clock_is_utc(bool is_utc)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(handle, NVS_CLOCK_KEY, is_utc ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret == ESP_OK) s_clock_is_utc.store(is_utc ? 1 : 0);
    return ret;
}

extern "C" bool time_basis_use_24_hour(void)
{
    const int cached = s_use_24_hour.load();
    if (cached >= 0) return cached != 0;
    uint8_t loaded = 1;
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, NVS_HOUR_KEY, &loaded);
        nvs_close(handle);
    }
    int expected = -1;
    s_use_24_hour.compare_exchange_strong(expected, loaded != 0 ? 1 : 0);
    return s_use_24_hour.load() != 0;
}

extern "C" esp_err_t time_basis_set_use_24_hour(bool enabled)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(handle, NVS_HOUR_KEY, enabled ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret == ESP_OK) s_use_24_hour.store(enabled ? 1 : 0);
    return ret;
}

extern "C" bool time_basis_system_clock_valid(void)
{
    return std::time(nullptr) >= 1704067200; // 2024-01-01 UTC
}

extern "C" bool time_basis_format_clock(int64_t utc_seconds, char *display,
                                           size_t capacity)
{
    if (!display || capacity == 0 || utc_seconds < 1704067200) return false;
    int64_t local = utc_seconds + time_basis_get_offset_minutes() * 60;
    int64_t seconds_in_day = local % 86400;
    if (seconds_in_day < 0) seconds_in_day += 86400;
    const int hour = static_cast<int>(seconds_in_day / 3600);
    const int minute = static_cast<int>(seconds_in_day / 60 % 60);
    const int written = time_basis_use_24_hour()
        ? std::snprintf(display, capacity, "%02d:%02d", hour, minute)
        : std::snprintf(display, capacity, "%02d:%02d %s",
                        hour % 12 == 0 ? 12 : hour % 12, minute,
                        hour < 12 ? "AM" : "PM");
    return written >= 0 && static_cast<size_t>(written) < capacity;
}

extern "C" bool time_basis_parse_modem(const char *raw, int64_t *utc_seconds)
{
    return parse_modem(raw, false, utc_seconds);
}

extern "C" bool time_basis_parse_modem_clock(const char *raw,
                                                int64_t *utc_seconds)
{
    return parse_modem(raw, time_basis_modem_clock_is_utc(), utc_seconds);
}

extern "C" bool time_basis_parse_beijing_key(int64_t key, int64_t *utc_seconds)
{
    const int year = static_cast<int>(key / 10000000000LL);
    const int month = static_cast<int>(key / 100000000LL % 100);
    const int day = static_cast<int>(key / 1000000LL % 100);
    const int hour = static_cast<int>(key / 10000LL % 100);
    const int minute = static_cast<int>(key / 100LL % 100);
    const int second = static_cast<int>(key % 100);
    return make_utc(year, month, day, hour, minute, second,
                    DEFAULT_OFFSET, utc_seconds);
}

extern "C" bool time_basis_parse_legacy_beijing(const char *display,
                                                    int64_t *utc_seconds)
{
    if (!display || std::strlen(display) != 16 || display[4] != '/' ||
        display[7] != '/' || display[10] != ' ' || display[13] != ':') return false;
    int year = 0;
    for (int i = 0; i < 4; ++i) {
        if (display[i] < '0' || display[i] > '9') return false;
        year = year * 10 + display[i] - '0';
    }
    return make_utc(year, two_digits(display, 5), two_digits(display, 8),
                    two_digits(display, 11), two_digits(display, 14), 0,
                    DEFAULT_OFFSET, utc_seconds);
}

extern "C" bool time_basis_format_utc(int64_t utc_seconds, char *display,
                                         size_t capacity, int64_t *sort_key)
{
    if (!display || capacity == 0) return false;
    int64_t local = utc_seconds + time_basis_get_offset_minutes() * 60;
    int64_t days = local / 86400;
    int64_t remainder = local % 86400;
    if (remainder < 0) { remainder += 86400; --days; }
    int year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, &year, &month, &day);
    const int hour = static_cast<int>(remainder / 3600);
    const int minute = static_cast<int>(remainder / 60 % 60);
    const int second = static_cast<int>(remainder % 60);
    const int written = std::snprintf(display, capacity, "%04d/%02u/%02u %02d:%02d",
                                      year, month, day, hour, minute);
    if (written < 0 || static_cast<size_t>(written) >= capacity) return false;
    if (sort_key) *sort_key = static_cast<int64_t>(year) * 10000000000LL +
        static_cast<int64_t>(month) * 100000000LL +
        static_cast<int64_t>(day) * 1000000LL + hour * 10000LL + minute * 100LL + second;
    return true;
}
