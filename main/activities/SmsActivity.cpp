#include "activities/SmsActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/TextInputActivity.h"
#include "modem/ml307_sms.h"
#include "time/time_basis.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/TextInputOverlay.h"
#include "ui/UiLocale.h"

#include <M5Unified.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "SMS_UI";
constexpr int HEADER_H = header_layout::height;
constexpr int FOOTER_H = 86;
constexpr int ROW_COUNT = 5;
constexpr int SIDE_PAD = 22;
constexpr int BUBBLE_MARGIN = 34;
constexpr int BUBBLE_PAD_X = 20;
constexpr int BUBBLE_PAD_Y = 14;
constexpr int BUBBLE_GAP = 16;
constexpr int BUBBLE_LINE_GAP = 3;
constexpr size_t UI_MAX_MESSAGES = ML307_SMS_MAX_MESSAGES * 2;
constexpr const char *SENT_STORE_DIR = "/sdcard/.reader/sms";
constexpr const char *SENT_STORE_PATH = "/sdcard/.reader/sms/sent.tsv";
constexpr const char *SENT_STORE_TEMP_PATH = "/sdcard/.reader/sms/sent.tsv.tmp";
constexpr const char *SENT_STORE_BACKUP_PATH = "/sdcard/.reader/sms/sent.tsv.bak";

enum class WorkKind { List, Send, Delete };

struct WorkResult {
    WorkKind kind;
    esp_err_t error;
    size_t count;
    ml307_sms_message_t messages[UI_MAX_MESSAGES];
};

int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    return value >= 'A' && value <= 'F' ? value - 'A' + 10 : -1;
}

std::string hex_encode(const char *text)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    for (const uint8_t *p = reinterpret_cast<const uint8_t *>(text); *p; ++p) {
        output.push_back(digits[*p >> 4]);
        output.push_back(digits[*p & 0x0f]);
    }
    return output;
}

std::string hex_decode(const std::string &text)
{
    if ((text.size() & 1U) != 0) return {};
    std::string output;
    output.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = hex_digit(text[i]);
        const int low = hex_digit(text[i + 1]);
        if (high < 0 || low < 0) return {};
        output.push_back(static_cast<char>((high << 4) | low));
    }
    return output;
}

constexpr int64_t UNKNOWN_SENT_TIME = std::numeric_limits<int64_t>::max();

void recover_sent_store()
{
    FILE *file = std::fopen(SENT_STORE_PATH, "rb");
    if (file) {
        std::fclose(file);
        return;
    }
    if (std::rename(SENT_STORE_BACKUP_PATH, SENT_STORE_PATH) == 0) {
        ESP_LOGW(TAG, "Recovered interrupted sent-message history replacement");
    }
}

bool append_sent_record(const char *address, const char *text, int64_t utc_seconds)
{
    recover_sent_store();
    mkdir("/sdcard/.reader", 0775);
    mkdir(SENT_STORE_DIR, 0775);
    FILE *file = std::fopen(SENT_STORE_PATH, "ab");
    if (!file) return false;
    const std::string address_hex = hex_encode(address);
    const std::string text_hex = hex_encode(text);
    // T is a UTC epoch timestamp. E/B records remain readable below.
    const bool ok = utc_seconds != 0
        ? std::fprintf(file, "T%lld\t%s\t%s\n",
                       static_cast<long long>(utc_seconds), address_hex.c_str(),
                       text_hex.c_str()) > 0
        : std::fprintf(file, "U\t%s\t%s\n", address_hex.c_str(),
                       text_hex.c_str()) > 0;
    const bool closed = std::fclose(file) == 0;
    return ok && closed;
}

void load_sent_records(WorkResult *result)
{
    recover_sent_store();
    FILE *file = std::fopen(SENT_STORE_PATH, "rb");
    if (!file) return;
    char line[ML307_SMS_TEXT_SIZE * 2 + ML307_SMS_ADDRESS_SIZE * 2 + 64];
    size_t line_number = 0;
    while (result->count < UI_MAX_MESSAGES && std::fgets(line, sizeof(line), file)) {
        const size_t current_line = line_number++;
        if (current_line > static_cast<size_t>(INT_MAX - 2)) break;
        char *first = std::strchr(line, '\t');
        if (!first) continue;
        char *second = std::strchr(first + 1, '\t');
        if (!second) continue;
        *first = '\0';
        *second = '\0';
        char *end = second + 1 + std::strcspn(second + 1, "\r\n");
        *end = '\0';
        int64_t utc_seconds = 0;
        if (line[0] == 'E' || line[0] == 'T') {
            char *parsed_end = nullptr;
            const int64_t parsed = std::strtoll(line + 1, &parsed_end, 10);
            if (parsed_end && *parsed_end == '\0' && parsed >= 1577836800LL) {
                // E records came from the previous +32 clock interpretation.
                utc_seconds = parsed + (line[0] == 'E' ? 8 * 3600 : 0);
            }
        } else if (line[0] == 'B' && std::strlen(line + 1) == 14) {
            if (time_basis_parse_beijing_key(std::strtoll(line + 1, nullptr, 10),
                                             &utc_seconds)) {
                utc_seconds += 8 * 3600;
            }
        }
        const std::string address = hex_decode(first + 1);
        const std::string text = hex_decode(second + 1);
        if (address.empty() || text.empty()) continue;
        auto &item = result->messages[result->count++];
        std::memset(&item, 0, sizeof(item));
        // Negative indices identify local file lines; modem indices are nonnegative.
        item.index = -2 - static_cast<int>(current_line);
        item.outgoing = true;
        item.sort_key = UNKNOWN_SENT_TIME;
        std::snprintf(item.address, sizeof(item.address), "%s", address.c_str());
        std::snprintf(item.text, sizeof(item.text), "%s", text.c_str());
        if (utc_seconds == 0 ||
            !time_basis_format_utc(utc_seconds, item.timestamp,
                                   sizeof(item.timestamp), &item.sort_key)) {
            std::snprintf(item.timestamp, sizeof(item.timestamp), "Time unknown");
        }
    }
    std::fclose(file);
}

esp_err_t delete_sent_record(int index, const char *address, const char *text)
{
    if (index > -2 || !address || !text) return ESP_ERR_INVALID_ARG;
    recover_sent_store();
    FILE *source = std::fopen(SENT_STORE_PATH, "rb");
    if (!source) return ESP_ERR_NOT_FOUND;
    FILE *target = std::fopen(SENT_STORE_TEMP_PATH, "wb");
    if (!target) {
        std::fclose(source);
        return ESP_FAIL;
    }
    const size_t selected_line = static_cast<size_t>(-2 - index);
    const std::string expected_address = hex_encode(address);
    const std::string expected_text = hex_encode(text);
    char line[ML307_SMS_TEXT_SIZE * 2 + ML307_SMS_ADDRESS_SIZE * 2 + 64];
    size_t line_number = 0;
    bool found = false;
    bool write_ok = true;
    while (std::fgets(line, sizeof(line), source)) {
        if (line_number++ == selected_line) {
            const char *first = std::strchr(line, '\t');
            const char *second = first ? std::strchr(first + 1, '\t') : nullptr;
            const char *end = second ? second + 1 + std::strcspn(second + 1, "\r\n") : nullptr;
            if (!first || !second || !end ||
                std::string(first + 1, second) != expected_address ||
                std::string(second + 1, end) != expected_text) {
                write_ok = false;
                break;
            }
            found = true;
        } else if (std::fputs(line, target) == EOF) {
            write_ok = false;
            break;
        }
    }
    if (std::ferror(source)) write_ok = false;
    if (std::fclose(source) != 0) write_ok = false;
    if (std::fflush(target) != 0) write_ok = false;
    if (std::fclose(target) != 0) write_ok = false;
    if (!found || !write_ok) {
        std::remove(SENT_STORE_TEMP_PATH);
        return found ? ESP_FAIL : ESP_ERR_NOT_FOUND;
    }
    if (std::remove(SENT_STORE_BACKUP_PATH) != 0 && errno != ENOENT) {
        std::remove(SENT_STORE_TEMP_PATH);
        return ESP_FAIL;
    }
    if (std::rename(SENT_STORE_PATH, SENT_STORE_BACKUP_PATH) != 0) {
        std::remove(SENT_STORE_TEMP_PATH);
        return ESP_FAIL;
    }
    if (std::rename(SENT_STORE_TEMP_PATH, SENT_STORE_PATH) != 0) {
        const int saved_errno = errno;
        if (std::rename(SENT_STORE_BACKUP_PATH, SENT_STORE_PATH) != 0) {
            ESP_LOGE(TAG, "Sent-message history rollback failed: errno=%d", errno);
        }
        std::remove(SENT_STORE_TEMP_PATH);
        ESP_LOGE(TAG, "Sent-message history replacement failed: errno=%d", saved_errno);
        return ESP_FAIL;
    }
    std::remove(SENT_STORE_BACKUP_PATH);
    ESP_LOGI(TAG, "Deleted local sent-message record line=%u",
             static_cast<unsigned>(selected_line));
    return ESP_OK;
}

struct WorkJob {
    WorkKind kind;
    int index;
    char address[ML307_SMS_ADDRESS_SIZE];
    char text[ML307_SMS_TEXT_SIZE];
    QueueHandle_t queue;
};

void sms_worker(void *argument)
{
    WorkJob *job = static_cast<WorkJob *>(argument);
    auto *result = static_cast<WorkResult *>(
        heap_caps_calloc(1, sizeof(WorkResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!result) result = static_cast<WorkResult *>(std::calloc(1, sizeof(WorkResult)));
    if (result) {
        result->kind = job->kind;
        if (job->kind == WorkKind::List) {
            result->error = ml307_sms_list(result->messages,
                                            ML307_SMS_MAX_MESSAGES,
                                            &result->count);
            if (result->error == ESP_OK) load_sent_records(result);
        } else if (job->kind == WorkKind::Send) {
            result->error = ml307_sms_send(job->address, job->text);
            if (result->error == ESP_OK) {
                int64_t utc_seconds = 0;
                if (ml307_sms_get_time_utc(&utc_seconds) != ESP_OK) {
                    ESP_LOGW(TAG, "SMS sent, but modem time unavailable; saving without timestamp");
                }
                if (!append_sent_record(job->address, job->text, utc_seconds)) {
                    ESP_LOGW(TAG, "SMS sent but local history append failed");
                }
            }
        } else {
            result->error = job->index <= -2
                ? delete_sent_record(job->index, job->address, job->text)
                : ml307_sms_delete(job->index);
        }
        if (xQueueSend(job->queue, &result, pdMS_TO_TICKS(1000)) != pdTRUE) {
            std::free(result);
        }
    }
    delete job;
    vTaskDelete(nullptr);
}

std::vector<std::string> wrap_text(const std::string &text, int width)
{
    std::vector<std::string> lines;
    std::string line;
    size_t offset = 0;
    while (offset < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        if (lead == '\r' || lead == '\n') {
            if (lead == '\r' && offset + 1 < text.size() && text[offset + 1] == '\n') {
                ++offset;
            }
            lines.push_back(line);
            line.clear();
            ++offset;
            continue;
        }
        size_t bytes = 1;
        if ((lead & 0xe0) == 0xc0) bytes = 2;
        else if ((lead & 0xf0) == 0xe0) bytes = 3;
        else if ((lead & 0xf8) == 0xf0) bytes = 4;
        bytes = std::min(bytes, text.size() - offset);
        const std::string glyph = text.substr(offset, bytes);
        if (!line.empty() && system_font::measure(line + glyph) > width) {
            lines.push_back(line);
            line.clear();
        }
        line += glyph;
        offset += bytes;
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.emplace_back();
    return lines;
}

std::string normalized_address(const char *address)
{
    std::string digits;
    if (address) {
        for (const char *p = address; *p; ++p) {
            if (*p >= '0' && *p <= '9') digits.push_back(*p);
        }
    }
    // Treat mainland local and +86/0086 representations as one conversation.
    if (digits.size() == 13 && digits.rfind("86", 0) == 0) digits.erase(0, 2);
    else if (digits.size() == 15 && digits.rfind("0086", 0) == 0) digits.erase(0, 4);
    return digits.empty() ? (address ? address : "") : digits;
}

struct Conversation {
    std::string key;
    std::string address;
    std::vector<size_t> messages;
    int64_t latest = 0;
    bool unread = false;
};

struct BubbleLayout {
    size_t message_index = 0;
    std::vector<std::string> lines;
    int top = 0;
    int height = 0;
    int width = 0;
    int x = 0;
};

class SmsActivity final : public Activity {
public:
    SmsActivity()
    {
        _queue = xQueueCreate(2, sizeof(WorkResult *));
    }

    ~SmsActivity() override
    {
        if (_queue) {
            WorkResult *result = nullptr;
            while (xQueueReceive(_queue, &result, 0) == pdTRUE) std::free(result);
            vQueueDelete(_queue);
        }
    }

    void on_create() override
    {
        draw_page(ui_locale::text(ui_locale::TextId::LoadingMessages));
        start_work(WorkKind::List);
    }

    void on_show() override
    {
        if (!_input.visible()) draw_page(_status);
    }

    void loop() override
    {
        if (!_queue) return;
        WorkResult *result = nullptr;
        if (xQueueReceive(_queue, &result, 0) == pdTRUE) {
            _busy = false;
            _task = nullptr;
            handle_result(*result);
            std::free(result);
        }
        if (!_busy && !_input.visible() && ml307_sms_take_new_message_hint()) {
            _status = ui_locale::text(ui_locale::TextId::NewMessageRefreshing);
            draw_page(_status);
            start_work(WorkKind::List);
        }
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (_input.visible()) return _input.handle_touch(event);
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y, 120))) {
            if (_detail >= 0) {
                _detail = -1;
                _selected_message = -1;
                draw_page("");
            } else if (!_busy) {
                ActivityManager::instance().pop();
            } else {
                _status = ui_locale::text(ui_locale::TextId::WaitForModem);
                draw_page(_status);
            }
            return true;
        }
        if (event.type == TouchEventType::SwipeUp) {
            if (_detail >= 0) change_thread(1);
            else change_page(1);
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            if (_detail >= 0) change_thread(-1);
            else change_page(-1);
            return true;
        }
        if (event.type != TouchEventType::Tap || _busy) return false;
        const int height = M5.Display.height();
        if (event.y >= height - FOOTER_H) {
            if (_detail >= 0) {
                if (event.x < M5.Display.width() / 2) {
                    start_delete();
                } else {
                    const auto &conversation = _conversations[static_cast<size_t>(_detail)];
                    open_message_input(conversation.address);
                }
            } else if (event.x < M5.Display.width() / 2) {
                _status = ui_locale::text(ui_locale::TextId::Refreshing);
                draw_page(_status);
                start_work(WorkKind::List);
            } else {
                open_address_input();
            }
            return true;
        }
        if (_detail < 0 && event.y >= HEADER_H) {
            const int row_h = (height - HEADER_H - FOOTER_H) / ROW_COUNT;
            const int slot = (event.y - HEADER_H) / std::max(1, row_h);
            const int index = _page * ROW_COUNT + slot;
            if (index >= 0 && index < static_cast<int>(_conversations.size())) {
                _detail = index;
                auto &conversation = _conversations[static_cast<size_t>(index)];
                _selected_message = -1;
                for (size_t message_index : conversation.messages) {
                    _messages[message_index].unread = false;
                }
                rebuild_bubbles();
                _thread_scroll = max_thread_scroll();
                draw_page("");
                return true;
            }
        } else if (_detail >= 0 && event.y >= HEADER_H &&
                   event.y < height - FOOTER_H) {
            const int virtual_y = event.y - HEADER_H + _thread_scroll;
            for (size_t i = 0; i < _bubbles.size(); ++i) {
                const auto &bubble = _bubbles[i];
                if (virtual_y >= bubble.top && virtual_y < bubble.top + bubble.height &&
                    event.x >= bubble.x && event.x < bubble.x + bubble.width) {
                    _selected_message = _selected_message == static_cast<int>(i)
                        ? -1 : static_cast<int>(i);
                    draw_page("");
                    return true;
                }
            }
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (_input.visible()) return _input.handle_key(key);
        if (key == APP_KEY_BACK) {
            TouchEvent event;
            event.type = TouchEventType::Back;
            return on_touch(event);
        }
        if (key == APP_KEY_UP) {
            if (_detail >= 0) change_thread(-1); else change_page(-1);
            return true;
        }
        if (key == APP_KEY_DOWN) {
            if (_detail >= 0) change_thread(1); else change_page(1);
            return true;
        }
        if (key == APP_KEY_OK && !_busy) { open_address_input(); return true; }
        return false;
    }

private:
    QueueHandle_t _queue = nullptr;
    TaskHandle_t _task = nullptr;
    ui::TextInputOverlay _input;
    std::vector<ml307_sms_message_t> _messages;
    std::vector<Conversation> _conversations;
    std::string _status;
    std::string _compose_address;
    int _page = 0;
    int _detail = -1;
    int _thread_scroll = 0;
    int _thread_height = 0;
    int _selected_message = -1;
    std::vector<BubbleLayout> _bubbles;
    bool _busy = false;

    void handle_result(const WorkResult &result)
    {
        if (result.error != ESP_OK) {
            _status = ml307_sms_is_ready()
                ? std::string(ui_locale::text(ui_locale::TextId::SmsOperationFailedPrefix)) +
                    esp_err_to_name(result.error)
                : ui_locale::text(ui_locale::TextId::SmsChannelNotReady);
            draw_page(_status);
            return;
        }
        if (result.kind == WorkKind::List) {
            const std::string open_key = _detail >= 0 &&
                _detail < static_cast<int>(_conversations.size())
                ? _conversations[static_cast<size_t>(_detail)].key : std::string();
            const bool was_at_bottom = _thread_scroll >= max_thread_scroll() - 8;
            _messages.assign(result.messages, result.messages + result.count);
            std::stable_sort(_messages.begin(), _messages.end(),
                      [](const auto &a, const auto &b) {
                          if (a.sort_key != b.sort_key) return a.sort_key < b.sort_key;
                          return a.index < b.index;
                      });
            rebuild_conversations();
            if (!open_key.empty()) {
                auto found = std::find_if(_conversations.begin(), _conversations.end(),
                    [&](const Conversation &item) { return item.key == open_key; });
                _detail = found == _conversations.end() ? -1
                    : static_cast<int>(found - _conversations.begin());
            }
            _selected_message = -1;
            rebuild_bubbles();
            if (was_at_bottom) _thread_scroll = max_thread_scroll();
            else _thread_scroll = std::min(_thread_scroll, max_thread_scroll());
            const int pages = std::max(1, static_cast<int>(
                (_conversations.size() + ROW_COUNT - 1) / ROW_COUNT));
            _page = std::min(_page, pages - 1);
            _status = _conversations.empty() ? ui_locale::text(ui_locale::TextId::NoMessages) : "";
        } else if (result.kind == WorkKind::Send) {
            _status = ui_locale::text(ui_locale::TextId::MessageSent);
        } else {
            _status = ui_locale::text(ui_locale::TextId::MessageDeleted);
            _selected_message = -1;
        }
        draw_page(_status);
        if (result.kind != WorkKind::List) start_work(WorkKind::List);
    }

    void rebuild_conversations()
    {
        _conversations.clear();
        std::map<std::string, size_t> positions;
        for (size_t i = 0; i < _messages.size(); ++i) {
            auto &message = _messages[i];
            std::string key = normalized_address(message.address);
            auto found = positions.find(key);
            if (found == positions.end()) {
                Conversation conversation;
                conversation.key = key;
                conversation.address = message.address[0] ? message.address : "Unknown sender";
                _conversations.push_back(std::move(conversation));
                found = positions.emplace(key, _conversations.size() - 1).first;
            }
            auto &conversation = _conversations[found->second];
            conversation.messages.push_back(i);
            conversation.latest = std::max(conversation.latest, message.sort_key);
            conversation.unread = conversation.unread || message.unread;
        }
        std::sort(_conversations.begin(), _conversations.end(),
                  [](const Conversation &a, const Conversation &b) {
                      return a.latest > b.latest;
                  });
    }

    int thread_view_height() const
    {
        return M5.Display.height() - HEADER_H - FOOTER_H;
    }

    int max_thread_scroll() const
    {
        return std::max(0, _thread_height - thread_view_height());
    }

    void rebuild_bubbles()
    {
        _bubbles.clear();
        _thread_height = 0;
        if (_detail < 0 || _detail >= static_cast<int>(_conversations.size())) return;
        const auto &conversation = _conversations[static_cast<size_t>(_detail)];
        const int screen_w = M5.Display.width();
        const int max_bubble_w = screen_w * 2 / 3;
        const int text_w = max_bubble_w - BUBBLE_PAD_X * 2;
        const int line_step = system_font::line_height() + BUBBLE_LINE_GAP;
        int top = BUBBLE_GAP;
        for (size_t index : conversation.messages) {
            const auto &message = _messages[index];
            BubbleLayout bubble;
            bubble.message_index = index;
            bubble.lines = wrap_text(message.text, text_w);
            int widest = 0;
            for (const auto &line : bubble.lines) {
                widest = std::max(widest, system_font::measure(line));
            }
            bubble.width = std::min(max_bubble_w,
                std::max(160, widest + BUBBLE_PAD_X * 2));
            bubble.x = message.outgoing ? screen_w - BUBBLE_MARGIN - bubble.width
                                        : BUBBLE_MARGIN;
            bubble.top = top;
            bubble.height = BUBBLE_PAD_Y * 2 +
                static_cast<int>(bubble.lines.size()) * line_step - BUBBLE_LINE_GAP;
            bubble.height = std::max(bubble.height, system_font::line_height() * 2 + 8);
            top += bubble.height + BUBBLE_GAP;
            _bubbles.push_back(std::move(bubble));
        }
        _thread_height = top;
    }

    void start_work(WorkKind kind, int index = -1,
                    const std::string &address = {}, const std::string &text = {})
    {
        if (_busy || !_queue) return;
        auto *job = new WorkJob{};
        job->kind = kind;
        job->index = index;
        job->queue = _queue;
        std::snprintf(job->address, sizeof(job->address), "%s", address.c_str());
        std::snprintf(job->text, sizeof(job->text), "%s", text.c_str());
        _busy = true;
        BaseType_t created = xTaskCreatePinnedToCore(
            sms_worker, "ml307_sms_ui", 7168, job, 4, &_task, 0);
        if (created != pdPASS) {
            delete job;
            _task = nullptr;
            _busy = false;
            _status = ui_locale::text(ui_locale::TextId::SmsTaskStartFailed);
            draw_page(_status);
        }
    }

    void start_delete()
    {
        if (_detail < 0 || _detail >= static_cast<int>(_conversations.size())) return;
        if (_selected_message < 0 ||
            _selected_message >= static_cast<int>(_bubbles.size())) {
            _status = ui_locale::text(ui_locale::TextId::TapMessageToSelect);
            draw_page(_status);
            return;
        }
        const auto &message = _messages[
            _bubbles[static_cast<size_t>(_selected_message)].message_index];
        if ((message.outgoing && message.index > -2) ||
            (!message.outgoing && message.index < 0)) {
            _status = ui_locale::text(ui_locale::TextId::CannotLocateMessage);
            draw_page(_status);
            return;
        }
        _status = ui_locale::text(ui_locale::TextId::Deleting);
        draw_page(_status);
        start_work(WorkKind::Delete, message.index, message.address, message.text);
    }

    void open_address_input()
    {
        TextInputOptions options;
        options.title = ui_locale::text(ui_locale::TextId::RecipientPhoneNumber);
        options.submit_label = ui_locale::text(ui_locale::TextId::Next);
        options.max_bytes = ML307_SMS_ADDRESS_SIZE - 1;
        options.start_in_chinese = false;
        _input.open(options, [this](const std::string &address) {
            _compose_address = address;
            open_message_input(address);
        }, [this]() { draw_page(_status); });
    }

    void open_message_input(const std::string &address)
    {
        _compose_address = address;
        TextInputOptions options;
        options.title = std::string(ui_locale::text(ui_locale::TextId::MessageToPrefix)) + address;
        options.submit_label = ui_locale::text(ui_locale::TextId::Send);
        options.max_bytes = ML307_SMS_TEXT_SIZE - 1;
        options.start_in_chinese = true;
        _input.open(options, [this](const std::string &text) {
            _status = ui_locale::text(ui_locale::TextId::Sending);
            draw_page(_status);
            start_work(WorkKind::Send, -1, _compose_address, text);
        }, [this]() { draw_page(_status); });
    }

    void change_page(int delta)
    {
        const int pages = std::max(1, static_cast<int>(
            (_conversations.size() + ROW_COUNT - 1) / ROW_COUNT));
        const int target = std::max(0, std::min(pages - 1, _page + delta));
        if (target == _page) return;
        _page = target;
        draw_page("");
    }

    void change_thread(int delta)
    {
        const int step = std::max(120, thread_view_height() * 3 / 4);
        const int target = std::max(0, std::min(max_thread_scroll(),
                                              _thread_scroll + delta * step));
        if (target == _thread_scroll) return;
        _thread_scroll = target;
        draw_page("");
    }

    void draw_header(const char *title)
    {
        M5.Display.fillRect(0, 0, M5.Display.width(), HEADER_H, TFT_WHITE);
        system_font::draw_left(M5.Display, "<", 24, header_layout::text_y(), TFT_BLACK);
        system_font::draw_center(M5.Display, title, M5.Display.width() / 2, header_layout::text_y(), TFT_BLACK);
        header_layout::draw_separator(M5.Display);
    }

    void draw_footer(const char *left, const char *right)
    {
        const int y = M5.Display.height() - FOOTER_H;
        M5.Display.drawFastHLine(0, y, M5.Display.width(), TFT_BLACK);
        system_font::draw_center(M5.Display, left, M5.Display.width() / 4, y + 18, TFT_BLACK);
        system_font::draw_center(M5.Display, right, M5.Display.width() * 3 / 4, y + 18, TFT_BLACK);
        M5.Display.drawFastVLine(M5.Display.width() / 2, y + 10, FOOTER_H - 20, TFT_BLACK);
    }

    void draw_list()
    {
        using ui_locale::TextId;
        using ui_locale::text;
        const int height = M5.Display.height();
        const int row_h = (height - HEADER_H - FOOTER_H) / ROW_COUNT;
        const int begin = _page * ROW_COUNT;
        for (int slot = 0; slot < ROW_COUNT; ++slot) {
            const int y = HEADER_H + slot * row_h;
            M5.Display.fillRect(0, y, M5.Display.width(), row_h, TFT_WHITE);
            const int index = begin + slot;
            if (index < static_cast<int>(_conversations.size())) {
                const auto &conversation = _conversations[static_cast<size_t>(index)];
                const auto &message = _messages[conversation.messages.back()];
                const std::string address = conversation.address == "Unknown sender"
                    ? text(TextId::UnknownSender) : conversation.address;
                const std::string preview = message.text[0] ? message.text : text(TextId::EmptyMessage);
                if (conversation.unread) M5.Display.fillCircle(SIDE_PAD + 6, y + 28, 6, TFT_BLACK);
                system_font::draw_left(M5.Display,
                    system_font::ellipsize(address, M5.Display.width() - 250),
                    SIDE_PAD + 24, y + 12, TFT_BLACK);
                const std::string list_time = std::strlen(message.timestamp) == 16
                    ? std::string(message.timestamp + 5)
                    : (std::strcmp(message.timestamp, "Time unknown") == 0
                           ? text(TextId::TimeUnknown) : message.timestamp);
                system_font::draw_right(M5.Display, list_time,
                    M5.Display.width() - SIDE_PAD, y + 12, TFT_DARKGRAY);
                const std::string direction = message.outgoing ? text(TextId::MePrefix) : "";
                system_font::draw_left(M5.Display,
                    system_font::ellipsize(direction + preview,
                        M5.Display.width() - SIDE_PAD * 2 - 90),
                    SIDE_PAD + 24, y + 60, TFT_DARKGRAY);
                char count[24];
                std::snprintf(count, sizeof(count), "%u",
                    static_cast<unsigned>(conversation.messages.size()));
                system_font::draw_right(M5.Display, count,
                    M5.Display.width() - SIDE_PAD, y + 60, TFT_DARKGRAY);
            }
            M5.Display.drawFastHLine(SIDE_PAD, y + row_h - 1,
                                     M5.Display.width() - SIDE_PAD * 2, TFT_BLACK);
        }
        const int pages = std::max(1, static_cast<int>(
            (_conversations.size() + ROW_COUNT - 1) / ROW_COUNT));
        char title[64];
        std::snprintf(title, sizeof(title), "%s  %d/%d", text(TextId::Messages), _page + 1, pages);
        draw_header(title);
        draw_footer(text(TextId::RefreshAction), text(TextId::Compose));
    }

    void draw_detail()
    {
        using ui_locale::TextId;
        using ui_locale::text;
        if (_detail < 0 || _detail >= static_cast<int>(_conversations.size())) return;
        const auto &conversation = _conversations[static_cast<size_t>(_detail)];
        char title[96];
        std::snprintf(title, sizeof(title), "%s  (%u)",
            conversation.address == "Unknown sender" ? text(TextId::UnknownSender)
                                                     : conversation.address.c_str(),
            static_cast<unsigned>(conversation.messages.size()));
        draw_header(title);
        const int view_bottom = M5.Display.height() - FOOTER_H;
        M5.Display.setClipRect(0, HEADER_H, M5.Display.width(), thread_view_height());
        const int line_step = system_font::line_height() + BUBBLE_LINE_GAP;
        for (size_t i = 0; i < _bubbles.size(); ++i) {
            const auto &bubble = _bubbles[i];
            const int y = HEADER_H + bubble.top - _thread_scroll;
            if (y >= view_bottom || y + bubble.height <= HEADER_H) continue;
            const auto &message = _messages[bubble.message_index];
            M5.Display.fillRoundRect(bubble.x, y, bubble.width, bubble.height,
                                     12, message.outgoing ? TFT_WHITE : TFT_LIGHTGRAY);
            M5.Display.drawRoundRect(bubble.x, y, bubble.width, bubble.height,
                                     12, TFT_BLACK);
            if (static_cast<int>(i) == _selected_message) {
                M5.Display.drawRoundRect(bubble.x + 3, y + 3,
                    bubble.width - 6, bubble.height - 6, 9, TFT_BLACK);
            }
            int text_y = y + BUBBLE_PAD_Y;
            for (const auto &line : bubble.lines) {
                system_font::draw_left(M5.Display, line,
                    bubble.x + BUBBLE_PAD_X, text_y, TFT_BLACK);
                text_y += line_step;
            }
            const std::string stamp = std::strcmp(message.timestamp, "Time unknown") == 0
                ? text(TextId::TimeUnknown) : message.timestamp;
            const size_t split = stamp.find(' ');
            const bool dated = split == 10 && stamp.size() == 16;
            const std::string date = dated ? stamp.substr(5, 5) : stamp;
            const std::string time = dated ? stamp.substr(split + 1) : "";
            const int time_y = y + bubble.height - system_font::line_height() * 2 - 4;
            if (message.outgoing) {
                system_font::draw_right(M5.Display, date, bubble.x - 8, time_y, TFT_DARKGRAY);
                if (!time.empty()) system_font::draw_right(M5.Display, time,
                    bubble.x - 8, time_y + system_font::line_height(), TFT_DARKGRAY);
            } else {
                const int time_x = bubble.x + bubble.width + 8;
                system_font::draw_left(M5.Display, date, time_x, time_y, TFT_DARKGRAY);
                if (!time.empty()) system_font::draw_left(M5.Display, time,
                    time_x, time_y + system_font::line_height(), TFT_DARKGRAY);
            }
        }
        M5.Display.clearClipRect();
        draw_footer(text(TextId::Delete), text(TextId::Reply));
    }

    void draw_page(const std::string &status)
    {
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        if (_detail >= 0) draw_detail();
        else draw_list();
        if (!status.empty()) {
            const int y = M5.Display.height() - FOOTER_H - 64;
            M5.Display.fillRect(0, y, M5.Display.width(), 62, TFT_WHITE);
            system_font::draw_center(M5.Display,
                system_font::ellipsize(status, M5.Display.width() - 40),
                M5.Display.width() / 2, y + 8, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

}  // namespace

void launch_sms()
{
    ActivityManager::instance().push(new SmsActivity());
}
