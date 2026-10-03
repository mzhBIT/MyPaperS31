#include "activities/PhoneActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/PhoneRingtonePickerActivity.h"
#include "audio/phone_ringtone.h"
#include "modem/ml307_sms.h"
#include "modem/ml307_voice.h"
#include "time/time_basis.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"

#include <M5Unified.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "PHONE_UI";
constexpr int HEADER_H = header_layout::height;
constexpr int TAB_BOTTOM = 148;
constexpr int KEYPAD_TOP = 390;
constexpr int KEYPAD_BOTTOM_MARGIN = 132;
constexpr const char *CALL_LOG_PATH = "/sdcard/.reader/calls.tsv";
bool s_phone_current = false;
class PhoneActivity;
PhoneActivity *s_phone_activity = nullptr;

enum class Operation : uint8_t { Query, Dial, Answer, Hangup, Tone };

struct Result {
    Operation operation;
    esp_err_t error;
    ml307_call_info_t call;
    char tone;
    int64_t utc_seconds;
};

struct Job {
    Operation operation;
    char number[48];
    char tone;
    QueueHandle_t queue;
    bool capture_time;
};

void phone_worker(void *argument)
{
    auto *job = static_cast<Job *>(argument);
    Result result{};
    result.operation = job->operation;
    result.tone = job->tone;
    switch (job->operation) {
        case Operation::Query: result.error = ml307_voice_query(&result.call); break;
        case Operation::Dial: result.error = ml307_voice_dial(job->number); break;
        case Operation::Answer: result.error = ml307_voice_answer(); break;
        case Operation::Hangup: result.error = ml307_voice_hangup(); break;
        case Operation::Tone: result.error = ml307_voice_send_dtmf(job->tone); break;
    }
    if (result.error == ESP_OK && job->capture_time &&
        (job->operation == Operation::Dial || job->operation == Operation::Answer ||
         (job->operation == Operation::Query && result.call.state != ML307_CALL_IDLE))) {
        const esp_err_t time_error = ml307_sms_get_time_utc(&result.utc_seconds);
        if (time_error != ESP_OK) {
            ESP_LOGW(TAG, "call time unavailable operation=%d: %s",
                     static_cast<int>(job->operation), esp_err_to_name(time_error));
        }
    }
    xQueueSend(job->queue, &result, pdMS_TO_TICKS(100));
    delete job;
    vTaskDelete(nullptr);
}

const char *call_status(ml307_call_state_t state)
{
    using ui_locale::TextId;
    using ui_locale::text;
    switch (state) {
        case ML307_CALL_DIALING: return text(TextId::Dialing);
        case ML307_CALL_ALERTING: return text(TextId::Ringing);
        case ML307_CALL_INCOMING: return text(TextId::IncomingCall);
        case ML307_CALL_ACTIVE: return text(TextId::Connected);
        case ML307_CALL_HELD: return text(TextId::OnHold);
        case ML307_CALL_WAITING: return text(TextId::CallWaiting);
        default: return text(TextId::ReadyToCall);
    }
}

const char *display_call_kind(const std::string &kind)
{
    using ui_locale::TextId;
    if (kind == "Received") return ui_locale::text(TextId::ReceivedCall);
    if (kind == "Missed") return ui_locale::text(TextId::MissedCall);
    if (kind == "Outgoing") return ui_locale::text(TextId::OutgoingCall);
    if (kind == "No answer") return ui_locale::text(TextId::NoAnswer);
    return kind.c_str();
}

struct CallRecord {
    std::string number;
    std::string timestamp;
    std::string kind;
    uint32_t duration_s = 0;
};

std::string display_call_time(const std::string &stored)
{
    int64_t utc_seconds = 0;
    if (stored.size() > 1 && (stored[0] == 'E' || stored[0] == 'T')) {
        char *end = nullptr;
        const int64_t parsed = std::strtoll(stored.c_str() + 1, &end, 10);
        if (end && *end == '\0' && parsed >= 1577836800LL) {
            // Earlier E records trusted ML307A's incorrect +32 CCLK suffix.
            utc_seconds = parsed + (stored[0] == 'E' ? 8 * 3600 : 0);
        }
    } else {
        if (time_basis_parse_legacy_beijing(stored.c_str(), &utc_seconds)) {
            utc_seconds += 8 * 3600;
        }
    }
    char formatted[32] = {};
    return utc_seconds && time_basis_format_utc(utc_seconds, formatted,
                                                 sizeof(formatted), nullptr)
        ? formatted : (stored == "Time unknown"
                           ? ui_locale::text(ui_locale::TextId::TimeUnknown) : stored);
}

std::string safe_log_field(std::string value)
{
    for (char &c : value) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return value;
}

void append_call_record(const CallRecord &record)
{
    mkdir("/sdcard/.reader", 0775);
    FILE *file = std::fopen(CALL_LOG_PATH, "ab");
    if (!file) {
        ESP_LOGW(TAG, "call history open failed");
        return;
    }
    const int written = std::fprintf(file, "%s\t%s\t%s\t%lu\n",
        safe_log_field(record.timestamp).c_str(),
        safe_log_field(record.kind).c_str(),
        safe_log_field(record.number).c_str(),
        static_cast<unsigned long>(record.duration_s));
    const int closed = std::fclose(file);
    if (written < 0 || closed != 0) {
        ESP_LOGW(TAG, "call history write failed");
    }
}

std::vector<CallRecord> load_call_records()
{
    std::deque<CallRecord> recent;
    FILE *file = std::fopen(CALL_LOG_PATH, "rb");
    if (!file) return {};
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        std::string row(line);
        const size_t a = row.find('\t');
        const size_t b = a == std::string::npos ? a : row.find('\t', a + 1);
        const size_t c = b == std::string::npos ? b : row.find('\t', b + 1);
        if (c == std::string::npos) continue;
        CallRecord entry;
        entry.timestamp = row.substr(0, a);
        entry.kind = row.substr(a + 1, b - a - 1);
        entry.number = row.substr(b + 1, c - b - 1);
        entry.duration_s = static_cast<uint32_t>(std::strtoul(row.c_str() + c + 1,
                                                               nullptr, 10));
        recent.push_front(std::move(entry));
        if (recent.size() > 100) recent.pop_back();
    }
    std::fclose(file);
    return {recent.begin(), recent.end()};
}

class PhoneActivity final : public Activity {
public:
    explicit PhoneActivity(bool incoming_hint) : _incoming_hint(incoming_hint)
    {
        _queue = xQueueCreate(1, sizeof(Result));
    }
    ~PhoneActivity() override
    {
        phone_ringtone_stop();
        s_phone_current = false;
        s_phone_activity = nullptr;
        if (_queue) vQueueDelete(_queue);
    }

    void on_create() override
    {
        if (_incoming_hint) {
            _ring_hint_until_us = esp_timer_get_time() + 2500000;
            phone_ringtone_start();
            _ringing = true;
        }
        char path[384]{};
        if (phone_ringtone_get_path(path, sizeof(path)) == ESP_OK) _ringtone_path = path;
        _records = load_call_records();
        start(Operation::Query);
    }

    void on_show() override
    {
        s_phone_current = true;
        s_phone_activity = this;
        draw();
    }
    void on_hide() override { s_phone_current = false; }
    void notify_ring()
    {
        _next_query_us = 0;
        _ring_hint_until_us = esp_timer_get_time() + 2500000;
        if (!_ringing) { phone_ringtone_start(); _ringing = true; }
    }

    void loop() override
    {
        Result result{};
        if (_queue && xQueueReceive(_queue, &result, 0) == pdTRUE) {
            _busy = false;
            const auto previous_state = _state;
            const auto previous_number = _number;
            const auto previous_status = _status;
            if (result.error == ESP_OK) {
                if (result.operation == Operation::Query) {
                    _state = result.call.state;
                    if (_state != ML307_CALL_IDLE && _call_started_us == 0) {
                        begin_call(_state == ML307_CALL_INCOMING ||
                                   _state == ML307_CALL_WAITING, result.utc_seconds);
                    }
                    if (_state == ML307_CALL_ACTIVE && _connected_us == 0) {
                        _connected_us = esp_timer_get_time();
                    }
                    if (_state == ML307_CALL_ACTIVE) _was_connected = true;
                    if (_state == ML307_CALL_IDLE && previous_state != ML307_CALL_IDLE) {
                        finish_call();
                        _keypad_visible = false;
                        _pending_tones.clear();
                        _tones.clear();
                    }
                    if (result.call.number[0]) _number = result.call.number;
                    _status.clear();
                    if (_state == ML307_CALL_INCOMING || _state == ML307_CALL_WAITING) {
                        if (!_ringing) { phone_ringtone_start(); _ringing = true; }
                    } else if (_state != ML307_CALL_IDLE ||
                               esp_timer_get_time() >= _ring_hint_until_us) {
                        if (_ringing) { phone_ringtone_stop(); _ringing = false; }
                    }
                } else if (result.operation == Operation::Dial) {
                    _state = ML307_CALL_DIALING;
                    begin_call(false, result.utc_seconds);
                    _status = ui_locale::text(ui_locale::TextId::Calling);
                } else if (result.operation == Operation::Answer) {
                    phone_ringtone_stop();
                    _ringing = false;
                    _state = ML307_CALL_ACTIVE;
                    if (_call_started_us == 0) begin_call(true, result.utc_seconds);
                    if (_connected_us == 0) _connected_us = esp_timer_get_time();
                    _was_connected = true;
                    _keypad_visible = false;
                    _status = ui_locale::text(ui_locale::TextId::Answered);
                } else if (result.operation == Operation::Hangup) {
                    phone_ringtone_stop();
                    _ringing = false;
                    _state = ML307_CALL_IDLE;
                    finish_call();
                    _keypad_visible = false;
                    _pending_tones.clear();
                    _tones.clear();
                    _status = ui_locale::text(ui_locale::TextId::CallEnded);
                } else {
                    _status = std::string(ui_locale::text(ui_locale::TextId::SentTonePrefix)) + result.tone;
                }
            } else {
                _status = std::string(ui_locale::text(result.operation == Operation::Tone
                    ? ui_locale::TextId::ToneFailedPrefix : ui_locale::TextId::ModemErrorPrefix)) +
                    esp_err_to_name(result.error);
                ESP_LOGW(TAG, "operation=%d failed: %s",
                         static_cast<int>(result.operation), esp_err_to_name(result.error));
            }
            _next_query_us = esp_timer_get_time() +
                (_state == ML307_CALL_IDLE ? 4000000 : 1200000);
            if (previous_state != _state || previous_number != _number ||
                previous_status != _status) draw();
        }
        if (!_busy && !_pending_tones.empty()) {
            if (_state == ML307_CALL_ACTIVE) {
                const char tone = _pending_tones.front();
                _pending_tones.pop_front();
                start(Operation::Tone, tone);
            } else {
                _pending_tones.clear();
            }
        }
        if (!_busy && _queue && esp_timer_get_time() >= _next_query_us &&
            ml307_sms_is_ready()) {
            start(Operation::Query);
        }
        if (_state == ML307_CALL_ACTIVE && !_show_history) {
            const uint32_t elapsed = call_duration_s();
            if (elapsed != _last_drawn_duration_s) draw_timer();
        }
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y, 120))) {
            if (!_busy && _state == ML307_CALL_IDLE) ActivityManager::instance().pop();
            return true;
        }
        if (_show_history && _state == ML307_CALL_IDLE &&
            (event.type == TouchEventType::SwipeUp ||
             event.type == TouchEventType::SwipeDown)) {
            const int page_size = history_page_size();
            const int max_offset = std::max(0, static_cast<int>(_records.size()) - page_size);
            _history_offset = std::clamp(_history_offset +
                (event.type == TouchEventType::SwipeUp ? page_size : -page_size),
                0, max_offset);
            draw();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        const int w = M5.Display.width();
        const int h = M5.Display.height();
        if (event.y >= HEADER_H && event.y < TAB_BOTTOM &&
            _state == ML307_CALL_IDLE) {
            _show_history = event.x >= w / 2;
            if (_show_history) _records = load_call_records();
            draw();
            return true;
        }
        if (_show_history && _state == ML307_CALL_IDLE) {
            if (event.y >= TAB_BOTTOM && event.y < h - 60) {
                const int row = (event.y - TAB_BOTTOM) / 115;
                const int index = _history_offset + row;
                if (index >= 0 && index < static_cast<int>(_records.size())) {
                    _number = _records[index].number;
                    _show_history = false;
                    draw();
                }
            }
            return true;
        }
        const int pad = 20;
        const int cols = 3;
        const int row_h = (h - KEYPAD_TOP - KEYPAD_BOTTOM_MARGIN) / 4;
        if (event.y >= KEYPAD_TOP && event.y < h - KEYPAD_BOTTOM_MARGIN &&
            (_state == ML307_CALL_IDLE ||
             (_state == ML307_CALL_ACTIVE && _keypad_visible))) {
            const int col = std::clamp((event.x - pad) * cols / (w - pad * 2), 0, 2);
            const int row = std::clamp((event.y - KEYPAD_TOP) / row_h, 0, 3);
            static constexpr char keys[4][4] = {"123", "456", "789", "*0#"};
            const char digit = keys[row][col];
            if (_state == ML307_CALL_ACTIVE) {
                if (_pending_tones.size() >= 16) {
                    _status = ui_locale::text(ui_locale::TextId::ToneQueueFull);
                } else {
                    _pending_tones.push_back(digit);
                    _tones.push_back(digit);
                    if (_tones.size() > 24) _tones.erase(0, _tones.size() - 24);
                    _status = std::string(ui_locale::text(ui_locale::TextId::TonePrefix)) + digit;
                }
            } else if (_number.size() < 32) {
                _number.push_back(digit);
            }
            draw();
            return true;
        }
        if (event.y >= h - KEYPAD_BOTTOM_MARGIN) {
            if (_state == ML307_CALL_ACTIVE && event.x < w / 2) {
                _keypad_visible = !_keypad_visible;
                draw();
                return true;
            }
            if (_busy) return true; // Finish the current AT command first.
            if (_state == ML307_CALL_INCOMING || _state == ML307_CALL_WAITING) {
                phone_ringtone_stop(); _ringing = false;
                if (event.x < w / 2) { _status = ui_locale::text(ui_locale::TextId::Answering); start(Operation::Answer); }
                else { _status = ui_locale::text(ui_locale::TextId::Rejecting); start(Operation::Hangup); }
            } else if (_state != ML307_CALL_IDLE) {
                _status = ui_locale::text(ui_locale::TextId::HangingUp);
                start(Operation::Hangup);
            } else if (event.x < w / 3) {
                if (!_number.empty()) _number.pop_back();
            } else if (event.x < w * 2 / 3) {
                if (!_number.empty()) { _status = ui_locale::text(ui_locale::TextId::Dialing); start(Operation::Dial); }
            } else if (_number.empty() || _number[0] != '+') {
                _number.insert(_number.begin(), '+');
            }
            draw();
            return true;
        }
        if (event.y >= 300 && event.y < KEYPAD_TOP && _state == ML307_CALL_IDLE) {
            if (event.x >= w * 3 / 4) {
                const esp_err_t ret = phone_ringtone_set_path("");
                if (ret == ESP_OK) _ringtone_path.clear();
                else _status = std::string(ui_locale::text(ui_locale::TextId::RingtonePrefix)) + esp_err_to_name(ret);
                draw();
            } else {
                launch_phone_ringtone_picker([this](const std::string &path) {
                    const esp_err_t ret = phone_ringtone_set_path(path.c_str());
                    if (ret == ESP_OK) { _ringtone_path = path; _status = ui_locale::text(ui_locale::TextId::RingtoneSaved); }
                    else _status = std::string(ui_locale::text(ui_locale::TextId::RingtonePrefix)) + esp_err_to_name(ret);
                });
            }
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            TouchEvent event{};
            event.type = TouchEventType::Back;
            return on_touch(event);
        }
        if (key == APP_KEY_OK && !_busy) {
            if (_state == ML307_CALL_INCOMING) {
                phone_ringtone_stop(); _ringing = false;
                start(Operation::Answer);
            }
            else if (_state == ML307_CALL_IDLE && !_number.empty()) start(Operation::Dial);
            else if (_state != ML307_CALL_IDLE) start(Operation::Hangup);
            return true;
        }
        return false;
    }

private:
    QueueHandle_t _queue = nullptr;
    bool _busy = false;
    Operation _operation = Operation::Query;
    int64_t _next_query_us = 0;
    ml307_call_state_t _state = ML307_CALL_IDLE;
    std::string _number;
    std::string _status;
    std::string _ringtone_path;
    std::string _tones;
    std::deque<char> _pending_tones;
    std::vector<CallRecord> _records;
    int _history_offset = 0;
    bool _show_history = false;
    bool _call_incoming = false;
    bool _was_connected = false;
    int64_t _call_started_us = 0;
    int64_t _connected_us = 0;
    uint32_t _last_drawn_duration_s = UINT32_MAX;
    std::string _call_timestamp;
    bool _keypad_visible = false;
    bool _incoming_hint = false;
    bool _ringing = false;
    int64_t _ring_hint_until_us = 0;

    int history_page_size() const
    {
        return std::max(1, static_cast<int>(
            (M5.Display.height() - TAB_BOTTOM - 50) / 115));
    }

    void begin_call(bool incoming, int64_t utc_seconds)
    {
        if (_call_started_us) return;
        _call_started_us = esp_timer_get_time();
        _call_incoming = incoming;
        _was_connected = false;
        _connected_us = 0;
        _last_drawn_duration_s = UINT32_MAX;
        _call_timestamp = utc_seconds > 0 ? "T" + std::to_string(utc_seconds)
                                          : "Time unknown";
        _show_history = false;
    }

    uint32_t call_duration_s() const
    {
        if (!_connected_us) return 0;
        return static_cast<uint32_t>(std::max<int64_t>(0,
            esp_timer_get_time() - _connected_us) / 1000000);
    }

    void finish_call()
    {
        if (!_call_started_us) return;
        CallRecord record;
        record.number = _number.empty() ? "Unknown caller" : _number;
        record.timestamp = _call_timestamp;
        record.kind = _call_incoming ? (_was_connected ? "Received" : "Missed") :
                                       (_was_connected ? "Outgoing" : "No answer");
        record.duration_s = call_duration_s();
        ESP_LOGI(TAG, "call history time stored=%s displayed=%s utc_offset_min=%d",
                 record.timestamp.c_str(), display_call_time(record.timestamp).c_str(),
                 time_basis_get_offset_minutes());
        append_call_record(record);
        _records.insert(_records.begin(), std::move(record));
        if (_records.size() > 100) _records.pop_back();
        _call_started_us = 0;
        _connected_us = 0;
        _was_connected = false;
        _last_drawn_duration_s = UINT32_MAX;
    }

    void draw_timer()
    {
        if (_state != ML307_CALL_ACTIVE || _show_history) return;
        const uint32_t seconds = call_duration_s();
        char label[24];
        std::snprintf(label, sizeof(label), "%02lu:%02lu:%02lu",
            static_cast<unsigned long>(seconds / 3600),
            static_cast<unsigned long>((seconds / 60) % 60),
            static_cast<unsigned long>(seconds % 60));
        const int w = M5.Display.width();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillRect(20, 296, w - 40, 48, TFT_WHITE);
        system_font::draw_center(M5.Display, label, w / 2, 299, TFT_BLACK);
        M5.Display.display(20, 296, w - 40, 48);
        M5.Display.endWrite();
        _last_drawn_duration_s = seconds;
    }

    void start(Operation operation, char tone = '\0')
    {
        if (_busy || !_queue) return;
        if (!ml307_sms_is_ready()) {
            _status = ui_locale::text(ui_locale::TextId::PhoneAtNotReady);
            _next_query_us = esp_timer_get_time() + 1500000;
            return;
        }
        auto *job = new Job{};
        job->operation = operation;
        job->tone = tone;
        job->capture_time = _call_started_us == 0 &&
            (operation == Operation::Dial || operation == Operation::Query ||
             operation == Operation::Answer);
        _operation = operation;
        job->queue = _queue;
        std::snprintf(job->number, sizeof(job->number), "%s", _number.c_str());
        _busy = true;
        if (xTaskCreatePinnedToCore(phone_worker, "ml307_phone", 6144, job, 4,
                                    nullptr, 0) != pdPASS) {
            delete job;
            _busy = false;
            _status = ui_locale::text(ui_locale::TextId::PhoneTaskUnavailable);
        }
    }

    void draw()
    {
        using ui_locale::TextId;
        using ui_locale::text;
        const int w = M5.Display.width();
        const int h = M5.Display.height();
        const int pad = 20;
        const int line_h = system_font::line_height();
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        system_font::draw_left(M5.Display, "<", pad, header_layout::text_y(), TFT_BLACK);
        system_font::draw_center(M5.Display, text(TextId::Phone), w / 2, header_layout::text_y(), TFT_BLACK);
        header_layout::draw_separator(M5.Display);
        system_font::draw_center(M5.Display, text(TextId::Dial), w / 4, HEADER_H + 14,
                                 _show_history ? TFT_DARKGRAY : TFT_BLACK);
        system_font::draw_center(M5.Display, text(TextId::CallHistory), w * 3 / 4,
                                 HEADER_H + 14,
                                 _show_history ? TFT_BLACK : TFT_DARKGRAY);
        M5.Display.drawFastHLine(_show_history ? w / 2 : pad, TAB_BOTTOM - 2,
                                 w / 2 - pad, TFT_BLACK);
        if (_show_history && _state == ML307_CALL_IDLE) {
            const int page_size = history_page_size();
            for (int row = 0; row < page_size; ++row) {
                const int index = _history_offset + row;
                if (index >= static_cast<int>(_records.size())) break;
                const CallRecord &entry = _records[index];
                const int y = TAB_BOTTOM + row * 115;
                system_font::draw_left(M5.Display,
                    system_font::ellipsize(entry.number == "Unknown caller"
                                               ? text(TextId::UnknownCaller) : entry.number,
                                           w - 2 * pad), pad, y + 6,
                    TFT_BLACK);
                char duration[20];
                std::snprintf(duration, sizeof(duration), "%02lu:%02lu:%02lu",
                    static_cast<unsigned long>(entry.duration_s / 3600),
                    static_cast<unsigned long>((entry.duration_s / 60) % 60),
                    static_cast<unsigned long>(entry.duration_s % 60));
                const std::string detail = std::string(display_call_kind(entry.kind)) + "  " +
                                           display_call_time(entry.timestamp) +
                                           "  " + duration;
                system_font::draw_left(M5.Display,
                    system_font::ellipsize(detail, w - 2 * pad), pad, y + 53,
                    TFT_DARKGRAY);
                M5.Display.drawFastHLine(pad, y + 113, w - pad * 2, TFT_DARKGRAY);
            }
            if (_records.empty()) system_font::draw_center(M5.Display,
                text(TextId::NoCallHistory), w / 2, TAB_BOTTOM + 100, TFT_DARKGRAY);
            M5.Display.display();
            M5.Display.endWrite();
            return;
        }
        system_font::draw_center(M5.Display, call_status(_state), w / 2, 164, TFT_BLACK);
        system_font::draw_center(M5.Display,
            system_font::ellipsize(_number.empty()
                ? (_state == ML307_CALL_IDLE ? text(TextId::EnterNumber) : text(TextId::UnknownCaller))
                : _number, w - 50),
            w / 2, 220, _number.empty() ? TFT_DARKGRAY : TFT_BLACK);
        if (!_status.empty()) system_font::draw_center(M5.Display,
            system_font::ellipsize(_status, w - 40), w / 2, 266, TFT_DARKGRAY);
        if (_state == ML307_CALL_ACTIVE) {
            const uint32_t seconds = call_duration_s();
            char duration[24];
            std::snprintf(duration, sizeof(duration), "%02lu:%02lu:%02lu",
                static_cast<unsigned long>(seconds / 3600),
                static_cast<unsigned long>((seconds / 60) % 60),
                static_cast<unsigned long>(seconds % 60));
            system_font::draw_center(M5.Display, duration, w / 2, 299, TFT_BLACK);
            _last_drawn_duration_s = seconds;
        }
        if (_state == ML307_CALL_IDLE) {
            std::string ringtone = _ringtone_path.empty() ? text(TextId::NoRingtone) :
                std::string(text(TextId::RingtonePrefix)) +
                    _ringtone_path.substr(_ringtone_path.find_last_of('/') + 1);
            system_font::draw_left(M5.Display,
                system_font::ellipsize(ringtone, w * 3 / 4 - pad * 2), pad, 324,
                TFT_BLACK);
            system_font::draw_center(M5.Display, text(TextId::Off), w * 7 / 8, 324, TFT_BLACK);
            M5.Display.drawFastHLine(pad, KEYPAD_TOP - 10, w - pad * 2, TFT_DARKGRAY);
        } else if (_state == ML307_CALL_ACTIVE && _keypad_visible) {
            system_font::draw_center(M5.Display,
                system_font::ellipsize(_tones.empty() ? text(TextId::ServiceOptionHint) : _tones,
                                       w - pad * 2), w / 2, 324, TFT_BLACK);
            M5.Display.drawFastHLine(pad, KEYPAD_TOP - 10, w - pad * 2, TFT_DARKGRAY);
        }
        if (_state == ML307_CALL_IDLE ||
            (_state == ML307_CALL_ACTIVE && _keypad_visible)) {
            const int row_h = (h - KEYPAD_TOP - KEYPAD_BOTTOM_MARGIN) / 4;
            const int col_w = (w - pad * 2) / 3;
            static constexpr char keys[4][4] = {"123", "456", "789", "*0#"};
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 3; ++col) {
                    const int x = pad + col * col_w;
                    const int y = KEYPAD_TOP + row * row_h;
                    char label[2] = {keys[row][col], 0};
                    system_font::draw_center(M5.Display, label,
                        x + col_w / 2, y + (row_h - line_h) / 2, TFT_BLACK);
                    if (col < 2) M5.Display.drawFastVLine(x + col_w, y + 16,
                        row_h - 32, TFT_DARKGRAY);
                }
                M5.Display.drawFastHLine(pad, KEYPAD_TOP + (row + 1) * row_h - 1,
                    w - pad * 2, TFT_DARKGRAY);
            }
        }
        const int footer_y = h - KEYPAD_BOTTOM_MARGIN;
        M5.Display.drawFastHLine(pad, footer_y, w - pad * 2, TFT_BLACK);
        if (_state == ML307_CALL_INCOMING || _state == ML307_CALL_WAITING) {
            system_font::draw_center(M5.Display, text(TextId::Answer), w / 4, footer_y + 30, TFT_BLACK);
            system_font::draw_center(M5.Display, text(TextId::Reject), w * 3 / 4, footer_y + 30, TFT_BLACK);
        } else if (_state == ML307_CALL_ACTIVE) {
            system_font::draw_center(M5.Display,
                _keypad_visible ? text(TextId::HideKeypad) : text(TextId::Keypad), w / 4,
                footer_y + 30, TFT_BLACK);
            system_font::draw_center(M5.Display, text(TextId::HangUp), w * 3 / 4,
                footer_y + 30, TFT_BLACK);
        } else if (_state != ML307_CALL_IDLE) {
            system_font::draw_center(M5.Display, text(TextId::HangUp), w / 2, footer_y + 30, TFT_BLACK);
        } else {
            system_font::draw_center(M5.Display, text(TextId::Delete), w / 6, footer_y + 30, TFT_BLACK);
            system_font::draw_center(M5.Display, text(TextId::Call), w / 2, footer_y + 30, TFT_BLACK);
            system_font::draw_center(M5.Display, "+", w * 5 / 6, footer_y + 30, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

} // namespace

void launch_phone(bool incoming_hint)
{
    ActivityManager::instance().push(new PhoneActivity(incoming_hint));
}
bool phone_activity_is_current()
{
    return s_phone_current;
}
void phone_activity_notify_ring()
{
    if (s_phone_current && s_phone_activity) s_phone_activity->notify_ring();
}
