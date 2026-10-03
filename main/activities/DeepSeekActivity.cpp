#include "activities/DeepSeekActivity.h"
#include "activities/DeepSeekImagePickerActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/SettingActivity.h"
#include "deepseek/DeepSeekClient.h"
#include "deepseek/DeepSeekConfig.h"
#include "deepseek/DeepSeekSessionStore.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/TextInputOverlay.h"
#include "ui/UiLocale.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char *TAG = "DEEPSEEK_UI";
constexpr int HEADER_H = header_layout::height;
constexpr int PAD = 20;
constexpr int SESSION_TOP = HEADER_H;
constexpr int SESSION_ROW_H = 104;
constexpr int CHAT_TOOLBAR_Y = HEADER_H;
constexpr int CHAT_TOOLBAR_H = 66;
constexpr int CHAT_BODY_Y = CHAT_TOOLBAR_Y + CHAT_TOOLBAR_H + 16;
constexpr int CHAT_INPUT_H = 86;
constexpr int CHAT_INPUT_Y_MARGIN = 20;

std::string image_label(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string display_session_title(const std::string &title)
{
    return title == "New Chat" ? ui_locale::text(ui_locale::TextId::NewChat) : title;
}

void draw_header(const std::string &title)
{
    int width = M5.Display.width();
    M5.Display.fillRect(0, 0, width, HEADER_H, TFT_WHITE);
    system_font::draw_left(M5.Display, "<", 18, header_layout::text_y(), TFT_BLACK);
    system_font::draw_center(M5.Display,
                             system_font::ellipsize(title, width - 150),
                             width / 2, header_layout::text_y(), TFT_BLACK);
    header_layout::draw_separator(M5.Display);
}

bool header_back_hit(const TouchEvent &event)
{
    return event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y, 110);
}

size_t utf8_char_bytes(const std::string &text, size_t pos)
{
    unsigned char c = static_cast<unsigned char>(text[pos]);
    if ((c & 0xe0) == 0xc0) return std::min<size_t>(2, text.size() - pos);
    if ((c & 0xf0) == 0xe0) return std::min<size_t>(3, text.size() - pos);
    if ((c & 0xf8) == 0xf0) return std::min<size_t>(4, text.size() - pos);
    return 1;
}

void wrap_text(const std::string &text, int max_width, std::vector<std::string> *lines)
{
    std::string line;
    for (size_t pos = 0; pos < text.size();) {
        if (text[pos] == '\r') {
            ++pos;
            continue;
        }
        if (text[pos] == '\n') {
            lines->push_back(line);
            line.clear();
            ++pos;
            continue;
        }
        size_t bytes = utf8_char_bytes(text, pos);
        std::string next = line + text.substr(pos, bytes);
        if (!line.empty() && system_font::measure(next) > max_width) {
            lines->push_back(line);
            line.clear();
        }
        line.append(text, pos, bytes);
        pos += bytes;
    }
    if (!line.empty() || text.empty()) lines->push_back(line);
}

struct ChatTaskEvent {
    bool complete = false;
    std::string chunk;
    DeepSeekClientResult result;
};

struct ChatTaskJob {
    DeepSeekConfig config;
    DeepSeekModel model = DeepSeekModel::Flash;
    bool thinking = false;
    std::vector<DeepSeekMessage> messages;
    QueueHandle_t event_queue = nullptr;
    const volatile bool *cancel_requested = nullptr;
    bool external_stack = false;
};

bool post_stream_chunk(void *context, const std::string &chunk)
{
    auto *job = static_cast<ChatTaskJob *>(context);
    auto *event = new ChatTaskEvent;
    event->chunk = chunk;
    if (xQueueSend(job->event_queue, &event, portMAX_DELAY) == pdTRUE) return true;
    delete event;
    return false;
}

void chat_task(void *argument)
{
    std::unique_ptr<ChatTaskJob> job(static_cast<ChatTaskJob *>(argument));
    const bool external_stack = job->external_stack;
    auto *event = new ChatTaskEvent;
    event->complete = true;
    event->result = DeepSeekClient::chat(
        job->config, job->model, job->thinking, job->messages,
        job->cancel_requested, post_stream_chunk, job.get());
    if (xQueueSend(job->event_queue, &event, portMAX_DELAY) != pdTRUE) delete event;
    if (external_stack) {
        vTaskDeleteWithCaps(nullptr);
    } else {
        vTaskDelete(nullptr);
    }
}

class DeepSeekActivity final : public Activity {
public:
    ~DeepSeekActivity() override
    {
        release_wifi();
        if (_event_queue) {
            ChatTaskEvent *event = nullptr;
            while (xQueueReceive(_event_queue, &event, 0) == pdTRUE) delete event;
            vQueueDelete(_event_queue);
        }
    }

    void on_create() override
    {
        _event_queue = xQueueCreate(2, sizeof(ChatTaskEvent *));
        load_sessions();
        draw_sessions(true);
        M5.Display.waitDisplay();
        if (!ensure_wifi()) {
            ESP_LOGW(TAG, "Wi-Fi unavailable on DeepSeek entry");
        }
    }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        if (_view == View::Chat) {
            if (_open_input_after_image) {
                _open_input_after_image = false;
                open_input();
                return;
            }
            draw_chat(false);
        } else {
            load_sessions();
            draw_sessions(false);
        }
    }

    void loop() override
    {
        if (!_pending_question.empty() && !_busy && !M5.Display.displayBusy()) {
            std::string question = std::move(_pending_question);
            _pending_question.clear();
            std::string image = std::move(_pending_image_path);
            _pending_image_path.clear();
            begin_request(question, image);
            return;
        }
        if (!_busy || !_event_queue || M5.Display.displayBusy()) return;
        ChatTaskEvent *raw_event = nullptr;
        if (xQueueReceive(_event_queue, &raw_event, 0) != pdTRUE) return;
        std::unique_ptr<ChatTaskEvent> event(raw_event);
        if (!event->complete) {
            if (!_exit_when_done) append_stream_chunk(event->chunk);
            return;
        }
        _busy = false;
        _task = nullptr;
        if (_exit_when_done) {
            ActivityManager::instance().pop();
            return;
        }
        const int preserved_start = visible_line_start();
        if (event->result.success) {
            if (_store.append_message(&_session, "assistant", event->result.content)) {
                _messages.push_back({"assistant", event->result.content, {}});
                _streaming_reply.clear();
                _streaming_active = false;
                _status.clear();
            } else {
                _status = ui_locale::text(ui_locale::TextId::ReplySaveFailed);
            }
        } else {
            _streaming_active = false;
            _status = event->result.canceled ? ui_locale::text(ui_locale::TextId::Canceled)
                                              : ui_locale::localize_message(event->result.error);
        }
        preserve_line_start(preserved_start);
        draw_chat(false);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (_text_input.visible()) return _text_input.handle_touch(event);
        if (_view == View::Sessions && _confirm_delete) {
            if (event.type == TouchEventType::Back || header_back_hit(event)) {
                cancel_session_delete();
                return true;
            }
            return session_delete_touch(event);
        }
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            go_back();
            return true;
        }
        if (_view == View::Sessions) return sessions_touch(event);
        return chat_touch(event);
    }

    bool on_key(int key) override
    {
        if (_text_input.visible()) return _text_input.handle_key(key);
        if (_view == View::Sessions && _confirm_delete) {
            if (key == APP_KEY_BACK) cancel_session_delete();
            else if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
                _delete_choice = 1 - _delete_choice;
                draw_sessions(false);
            } else if (key == APP_KEY_OK) {
                if (_delete_choice == 0) cancel_session_delete();
                else confirm_session_delete();
            }
            return true;
        }
        if (key == APP_KEY_BACK) {
            go_back();
            return true;
        }
        if (_view == View::Sessions) {
            if (key == APP_KEY_UP) move_session_focus(-1);
            else if (key == APP_KEY_DOWN) move_session_focus(1);
            else if (key == APP_KEY_OK) open_session_row(_session_focus);
            return true;
        }
        if (_busy && key != APP_KEY_UP && key != APP_KEY_DOWN) return true;
        if (key == APP_KEY_UP) scroll_chat(visible_chat_lines());
        else if (key == APP_KEY_DOWN) scroll_chat(-visible_chat_lines());
        else if (key == APP_KEY_OK) open_input();
        else if (key == APP_KEY_EXTRA) toggle_model();
        return true;
    }

private:
    enum class View { Sessions, Chat };

    View _view = View::Sessions;
    bool _seen_show = false;
    bool _busy = false;
    bool _cancel_requested = false;
    bool _exit_when_done = false;
    bool _wifi_active = false;
    int _session_focus = 0;
    int _session_scroll = 0;
    int _line_scroll = 0;
    TaskHandle_t _task = nullptr;
    QueueHandle_t _event_queue = nullptr;
    DeepSeekSessionStore _store;
    std::vector<DeepSeekSessionMeta> _sessions;
    DeepSeekSessionMeta _session;
    std::vector<DeepSeekMessage> _messages;
    std::string _pending_question;
    std::string _pending_image_path;
    bool _open_input_after_image = false;
    // The microphone is intentionally UI-only for now. Keeping this state here
    // makes the input bar ready for an audio-capable model without pretending
    // that a recording has started.
    bool _voice_input_mode = false;
    std::string _status;
    std::string _streaming_reply;
    bool _streaming_active = false;
    bool _confirm_delete = false;
    int _delete_choice = 0;
    int _delete_index = -1;
    std::string _delete_title;
    std::string _session_notice;
    ui::TextInputOverlay _text_input;

    bool ensure_wifi()
    {
        if (wifi_setting_is_connected()) {
            _wifi_active = true;
            return true;
        }
        const esp_err_t error =
            wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
        _wifi_active = error == ESP_OK;
        return _wifi_active;
    }

    void release_wifi()
    {
        if (!_wifi_active && !wifi_setting_is_connected()) return;
        _wifi_active = false;
        wifi_setting_release();
    }

    int visible_session_rows() const
    {
        return std::max(1, static_cast<int>((M5.Display.height() - SESSION_TOP - 24) /
                                             SESSION_ROW_H));
    }

    int session_row_count() const { return static_cast<int>(_sessions.size()) + 1; }

    void load_sessions()
    {
        _store.list(&_sessions);
        _session_focus = std::min(_session_focus, std::max(0, session_row_count() - 1));
        int max_scroll = std::max(0, session_row_count() - visible_session_rows());
        _session_scroll = std::min(_session_scroll, max_scroll);
    }

    void draw_sessions(bool quality)
    {
        int width = M5.Display.width();
        int rows = visible_session_rows();
        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::DeepSeek));
        for (int row = 0; row < rows; ++row) {
            int index = _session_scroll + row;
            if (index >= session_row_count()) break;
            int y = SESSION_TOP + row * SESSION_ROW_H;
            bool selected = index == _session_focus;
            uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
            uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRect(PAD, y, width - PAD * 2, SESSION_ROW_H - 4, bg);
            if (index == 0) {
                system_font::draw_left(M5.Display,
                                       std::string("+  ") + ui_locale::text(ui_locale::TextId::NewChat),
                                       PAD + 16, y + 24, fg);
            } else {
                const auto &session = _sessions[index - 1];
                system_font::draw_left(M5.Display,
                                       system_font::ellipsize(display_session_title(session.title), width - 230),
                                       PAD + 16, y + 8, fg);
                system_font::draw_left(M5.Display, deepseek_model_label(session.model),
                                       PAD + 16, y + 53, fg);
            }
            M5.Display.drawFastHLine(PAD, y + SESSION_ROW_H - 4,
                                     width - PAD * 2, TFT_BLACK);
        }
        if (!_session_notice.empty() && !_confirm_delete) {
            system_font::draw_center(M5.Display,
                                     system_font::ellipsize(_session_notice, width - PAD * 2),
                                     width / 2, M5.Display.height() - 54, TFT_BLACK);
        }
        if (_confirm_delete) draw_session_delete_dialog();
        M5.Display.display();
        M5.Display.endWrite();
    }

    void move_session_focus(int delta)
    {
        _session_focus = std::max(0, std::min(session_row_count() - 1,
                                             _session_focus + delta));
        int rows = visible_session_rows();
        if (_session_focus < _session_scroll) _session_scroll = _session_focus;
        if (_session_focus >= _session_scroll + rows) {
            _session_scroll = _session_focus - rows + 1;
        }
        draw_sessions(false);
    }

    bool sessions_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::SwipeUp) {
            move_session_focus(visible_session_rows());
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            move_session_focus(-visible_session_rows());
            return true;
        }
        if (event.type == TouchEventType::LongPress) {
            int index = session_index_at(event.y);
            if (index <= 0) return index == 0;
            begin_session_delete(index);
            return true;
        }
        if (event.type != TouchEventType::Tap || event.y < SESSION_TOP) return false;
        int index = session_index_at(event.y);
        if (index >= 0 && index < session_row_count()) {
            _session_focus = index;
            open_session_row(index);
            return true;
        }
        return false;
    }

    int session_index_at(int y) const
    {
        if (y < SESSION_TOP) return -1;
        const int row = (y - SESSION_TOP) / SESSION_ROW_H;
        if (row < 0 || row >= visible_session_rows()) return -1;
        const int index = _session_scroll + row;
        return index < session_row_count() ? index : -1;
    }

    void begin_session_delete(int index)
    {
        if (index <= 0 || index > static_cast<int>(_sessions.size())) return;
        _session_focus = index;
        _delete_index = index;
        _delete_title = _sessions[static_cast<size_t>(index - 1)].title;
        _delete_choice = 0;
        _confirm_delete = true;
        _session_notice.clear();
        draw_sessions(false);
    }

    void cancel_session_delete()
    {
        _confirm_delete = false;
        _delete_choice = 0;
        _delete_index = -1;
        _delete_title.clear();
        draw_sessions(false);
    }

    void confirm_session_delete()
    {
        if (_delete_index <= 0 || _delete_index > static_cast<int>(_sessions.size())) {
            cancel_session_delete();
            return;
        }
        const std::string id = _sessions[static_cast<size_t>(_delete_index - 1)].id;
        const bool removed = _store.remove(id);
        if (removed && _session.id == id) {
            _session = {};
            std::vector<DeepSeekMessage>().swap(_messages);
            _streaming_reply.clear();
            _streaming_active = false;
        }
        _confirm_delete = false;
        _delete_choice = 0;
        _delete_index = -1;
        _delete_title.clear();
        _session_notice = removed ? std::string()
                                  : ui_locale::text(ui_locale::TextId::DeleteConversationFailed);
        load_sessions();
        draw_sessions(false);
    }

    void draw_session_delete_dialog()
    {
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        const int box_width = std::min(width - 48, 620);
        const int box_height = 270;
        const int x = (width - box_width) / 2;
        const int y = (height - box_height) / 2;
        M5.Display.fillRoundRect(x, y, box_width, box_height, 8, TFT_WHITE);
        M5.Display.drawRoundRect(x, y, box_width, box_height, 8, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::DeleteConversation), width / 2,
                                 y + 20, TFT_BLACK);
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(display_session_title(_delete_title), box_width - 48),
                                 width / 2, y + 82, TFT_BLACK);

        const int gap = 18;
        const int button_height = 68;
        const int button_width = (box_width - 48 - gap) / 2;
        const int button_y = y + box_height - button_height - 22;
        const int left_x = x + 24;
        const int right_x = left_x + button_width + gap;
        M5.Display.fillRoundRect(left_x, button_y, button_width, button_height, 6,
                                 _delete_choice == 0 ? TFT_BLACK : TFT_WHITE);
        M5.Display.drawRoundRect(left_x, button_y, button_width, button_height, 6, TFT_BLACK);
        M5.Display.fillRoundRect(right_x, button_y, button_width, button_height, 6,
                                 _delete_choice == 1 ? TFT_BLACK : TFT_WHITE);
        M5.Display.drawRoundRect(right_x, button_y, button_width, button_height, 6, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Cancel), left_x + button_width / 2,
                                 button_y + 12,
                                 _delete_choice == 0 ? TFT_WHITE : TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Delete), right_x + button_width / 2,
                                 button_y + 12,
                                 _delete_choice == 1 ? TFT_WHITE : TFT_BLACK);
    }

    bool session_delete_touch(const TouchEvent &event)
    {
        if (event.type != TouchEventType::Tap) return true;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        const int box_width = std::min(width - 48, 620);
        const int box_height = 270;
        const int x = (width - box_width) / 2;
        const int y = (height - box_height) / 2;
        const int gap = 18;
        const int button_height = 68;
        const int button_width = (box_width - 48 - gap) / 2;
        const int button_y = y + box_height - button_height - 22;
        const int left_x = x + 24;
        const int right_x = left_x + button_width + gap;
        if (event.y < button_y || event.y > button_y + button_height) return true;
        if (event.x >= left_x && event.x <= left_x + button_width) {
            cancel_session_delete();
        } else if (event.x >= right_x && event.x <= right_x + button_width) {
            confirm_session_delete();
        }
        return true;
    }

    void open_session_row(int index)
    {
        _status.clear();
        _line_scroll = 0;
        if (index == 0) {
            DeepSeekConfig config;
            deepseek_config_load(&config);
            _session = {};
            _session.title = "New Chat";
            _session.model = config.default_model;
            _messages.clear();
        } else if (index <= static_cast<int>(_sessions.size())) {
            _session = _sessions[index - 1];
            if (!_store.load_messages(_session.id, &_messages)) {
                _status = ui_locale::text(ui_locale::TextId::ConversationLoadFailed);
                return;
            }
        } else {
            return;
        }
        _view = View::Chat;
        draw_chat(false);
    }

    struct TranscriptLine {
        std::string text;
        bool right_aligned = false;

        bool operator==(const TranscriptLine &other) const
        {
            return right_aligned == other.right_aligned && text == other.text;
        }
        bool operator!=(const TranscriptLine &other) const { return !(*this == other); }
    };

    static void append_wrapped_lines(std::vector<TranscriptLine> *output,
                                     const std::string &text,
                                     int max_width,
                                     bool right_aligned)
    {
        std::vector<std::string> wrapped;
        wrap_text(text, max_width, &wrapped);
        for (std::string &line : wrapped) {
            output->push_back({std::move(line), right_aligned});
        }
    }

    std::vector<TranscriptLine> transcript_lines() const
    {
        std::vector<TranscriptLine> lines;
        int max_width = M5.Display.width() - PAD * 2 - 12;
        for (const auto &message : _messages) {
            const bool user = message.role == "user";
            lines.push_back({ui_locale::text(user ? ui_locale::TextId::You
                                                  : ui_locale::TextId::DeepSeek), user});
            if (!message.image_path.empty()) {
                lines.push_back({std::string(ui_locale::text(ui_locale::TextId::ImagePrefix)) +
                                     image_label(message.image_path), user});
            }
            append_wrapped_lines(&lines, message.content, max_width, user);
            lines.push_back({});
        }
        if (_streaming_active || !_streaming_reply.empty()) {
            lines.push_back({ui_locale::text(ui_locale::TextId::DeepSeek), false});
            append_wrapped_lines(&lines, _streaming_reply, max_width, false);
            lines.push_back({});
        }
        if (!_status.empty() && !_busy) {
            lines.push_back({ui_locale::text(ui_locale::TextId::Status), false});
            append_wrapped_lines(&lines, _status, max_width, false);
        }
        return lines;
    }

    int visible_chat_lines() const
    {
        int bottom = M5.Display.height() - CHAT_INPUT_H - CHAT_INPUT_Y_MARGIN - 18;
        return std::max(1, (bottom - CHAT_BODY_Y) / system_font::line_height());
    }

    int visible_line_start() const
    {
        const int total = static_cast<int>(transcript_lines().size());
        return std::max(0, total - visible_chat_lines() - _line_scroll);
    }

    void preserve_line_start(int start)
    {
        const int total = static_cast<int>(transcript_lines().size());
        const int max_start = std::max(0, total - visible_chat_lines());
        start = std::clamp(start, 0, max_start);
        _line_scroll = max_start - start;
    }

    void append_stream_chunk(const std::string &chunk)
    {
        const int start = visible_line_start();
        const int visible = visible_chat_lines();
        std::vector<TranscriptLine> before_page;
        before_page.reserve(static_cast<size_t>(visible));
        {
            const std::vector<TranscriptLine> before = transcript_lines();
            for (int row = 0; row < visible; ++row) {
                const int index = start + row;
                before_page.push_back(index < static_cast<int>(before.size())
                    ? before[static_cast<size_t>(index)] : TranscriptLine{});
            }
        }
        _streaming_reply += chunk;
        preserve_line_start(start);
        const std::vector<TranscriptLine> after = transcript_lines();
        bool visible_changed = false;
        for (int row = 0; row < visible; ++row) {
            const int index = start + row;
            const TranscriptLine new_line = index < static_cast<int>(after.size())
                ? after[static_cast<size_t>(index)] : TranscriptLine{};
            if (before_page[static_cast<size_t>(row)] != new_line) {
                visible_changed = true;
                break;
            }
        }
        // Once the current page is full, later stream chunks are retained in
        // _streaming_reply without repeatedly refreshing an unchanged EPD page.
        if (visible_changed) draw_chat(false);
    }

    void draw_chat(bool quality)
    {
        int width = M5.Display.width();
        int height = M5.Display.height();
        int input_y = height - CHAT_INPUT_H - CHAT_INPUT_Y_MARGIN;
        std::vector<TranscriptLine> lines = transcript_lines();
        int visible = visible_chat_lines();
        int max_scroll = std::max(0, static_cast<int>(lines.size()) - visible);
        _line_scroll = std::max(0, std::min(_line_scroll, max_scroll));
        int start = std::max(0, static_cast<int>(lines.size()) - visible - _line_scroll);

        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(display_session_title(_session.title));

        constexpr int toolbar_gap = 8;
        int toolbar_width = (width - PAD * 2 - toolbar_gap * 2) / 3;
        const char *toolbar_labels[] = {
            ui_locale::text(ui_locale::TextId::NewChatShort),
            deepseek_model_label(_session.model),
            ui_locale::text(_session.thinking ? ui_locale::TextId::Think
                                              : ui_locale::TextId::NoThink),
        };
        for (int i = 0; i < 3; ++i) {
            int x = PAD + i * (toolbar_width + toolbar_gap);
            M5.Display.drawRoundRect(x, CHAT_TOOLBAR_Y, toolbar_width,
                                     CHAT_TOOLBAR_H, 7, TFT_BLACK);
            system_font::draw_center(M5.Display, toolbar_labels[i],
                                     x + toolbar_width / 2,
                                     CHAT_TOOLBAR_Y + 10, TFT_BLACK);
        }

        int y = CHAT_BODY_Y;
        for (int index = start; index < static_cast<int>(lines.size()) &&
                                  index < start + visible; ++index) {
            const TranscriptLine &line = lines[static_cast<size_t>(index)];
            if (line.right_aligned) {
                system_font::draw_right(M5.Display, line.text,
                                        width - PAD - 6, y, TFT_BLACK);
            } else {
                system_font::draw_left(M5.Display, line.text,
                                       PAD + 6, y, TFT_BLACK);
            }
            y += system_font::line_height();
        }

        constexpr int input_icon_width = 62;
        const int input_x = PAD;
        const int input_width = width - PAD * 2;
        const int image_x = input_x + input_width - input_icon_width * 2;
        const int voice_x = input_x + input_width - input_icon_width;
        const int icon_center_y = input_y + CHAT_INPUT_H / 2;
        M5.Display.fillRoundRect(input_x, input_y, input_width,
                                 CHAT_INPUT_H, 8, TFT_WHITE);
        M5.Display.drawRoundRect(input_x, input_y, input_width,
                                 CHAT_INPUT_H, 8, TFT_BLACK);
        M5.Display.drawFastVLine(image_x, input_y + 6, CHAT_INPUT_H - 12, TFT_BLACK);
        M5.Display.drawFastVLine(voice_x, input_y + 6, CHAT_INPUT_H - 12, TFT_BLACK);

        const std::string ready_label = ui_locale::text(
            _voice_input_mode ? ui_locale::TextId::HoldToTalk
            : _pending_image_path.empty() ? ui_locale::TextId::AskDeepSeek
                                          : ui_locale::TextId::ImageAttachedTypeMessage);
        const char *input_label = !_busy ? ready_label.c_str()
                                  : ui_locale::text(_session.thinking
                                        ? ui_locale::TextId::ThinkingDeeplyCancel
                                        : ui_locale::TextId::ThinkingCancel);
        const int label_width = std::max(1, image_x - input_x - 22);
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(input_label, label_width),
                               input_x + 12,
                               input_y + std::max(0, (CHAT_INPUT_H - system_font::line_height()) / 2),
                               _busy ? TFT_BLACK : TFT_DARKGRAY);

        // Attachment icon. It owns the former toolbar Image action.
        const int plus_x = image_x + input_icon_width / 2;
        M5.Display.drawFastHLine(plus_x - 13, icon_center_y, 27, TFT_BLACK);
        M5.Display.drawFastVLine(plus_x, icon_center_y - 13, 27, TFT_BLACK);

        // A deliberately passive microphone affordance: selecting it changes
        // the composer to "Hold to talk", while recording is added later.
        const int mic_x = voice_x + input_icon_width / 2;
        M5.Display.drawRoundRect(mic_x - 8, icon_center_y - 16, 16, 25, 8, TFT_BLACK);
        M5.Display.drawFastVLine(mic_x, icon_center_y + 9, 12, TFT_BLACK);
        M5.Display.drawFastHLine(mic_x - 11, icon_center_y + 21, 23, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool chat_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::SwipeUp) {
            scroll_chat(-visible_chat_lines());
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            scroll_chat(visible_chat_lines());
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (_busy) return true;
        if (event.y >= CHAT_TOOLBAR_Y && event.y < CHAT_TOOLBAR_Y + CHAT_TOOLBAR_H) {
            int screen_width = std::max(1, static_cast<int>(M5.Display.width()));
            int item = std::min(2, event.x * 3 / screen_width);
            if (item == 0) create_new_chat();
            else if (item == 1) toggle_model();
            else toggle_thinking();
            return true;
        }
        int input_y = M5.Display.height() - CHAT_INPUT_H - CHAT_INPUT_Y_MARGIN;
        if (event.y >= input_y) {
            constexpr int input_icon_width = 62;
            const int input_x = PAD;
            const int input_width = M5.Display.width() - PAD * 2;
            const int image_x = input_x + input_width - input_icon_width * 2;
            const int voice_x = input_x + input_width - input_icon_width;
            if (event.x >= voice_x) {
                toggle_voice_input();
            } else if (event.x >= image_x) {
                choose_image();
            } else if (!_voice_input_mode) {
                open_input();
            }
            return true;
        }
        return false;
    }

    void scroll_chat(int delta)
    {
        _line_scroll = std::max(0, _line_scroll + delta);
        draw_chat(false);
    }

    void create_new_chat()
    {
        DeepSeekConfig config;
        deepseek_config_load(&config);
        _session = {};
        _session.title = "New Chat";
        _session.model = config.default_model;
        _messages.clear();
        _pending_image_path.clear();
        _open_input_after_image = false;
        _voice_input_mode = false;
        _status.clear();
        _line_scroll = 0;
        draw_chat(false);
    }

    void toggle_model()
    {
        DeepSeekModel model = deepseek_model_toggle(_session.model);
        if (_session.id.empty()) {
            _session.model = model;
            draw_chat(false);
        } else if (!_store.set_model(&_session, model)) {
            _status = ui_locale::text(ui_locale::TextId::ModelSaveFailed);
            draw_chat(false);
        } else {
            draw_chat(false);
        }
    }

    void toggle_thinking()
    {
        bool enabled = !_session.thinking;
        if (_session.id.empty()) {
            _session.thinking = enabled;
            draw_chat(false);
        } else if (!_store.set_thinking(&_session, enabled)) {
            _status = ui_locale::text(ui_locale::TextId::ThinkingModeSaveFailed);
            draw_chat(false);
        } else {
            draw_chat(false);
        }
    }

    void open_input()
    {
        TextInputOptions options;
        options.title = std::string(ui_locale::text(ui_locale::TextId::AskModelPrefix)) +
                        deepseek_model_label(_session.model);
        options.submit_label = ui_locale::text(ui_locale::TextId::Send);
        options.max_bytes = 2048;
        options.start_in_chinese = true;
        _text_input.open(options, [this](const std::string &value) {
            // Wait for the keyboard overlay refresh to finish before touching
            // the SD-backed session store. The old full-screen input path got
            // this serialization naturally when its activity was popped.
            _pending_question = value;
        }, [this]() { draw_chat(false); });
    }

    void toggle_voice_input()
    {
        _voice_input_mode = !_voice_input_mode;
        draw_chat(false);
    }

    void choose_image()
    {
        if (_session.model != DeepSeekModel::Flash) {
            _status = ui_locale::text(ui_locale::TextId::ImageRequiresFlash);
            draw_chat(false);
            return;
        }
        _voice_input_mode = false;
        launch_deepseek_image_picker([this](const std::string &path) {
            _pending_image_path = path;
            _open_input_after_image = true;
            _status = std::string(ui_locale::text(ui_locale::TextId::ImageAttachedPrefix)) +
                      image_label(path);
        });
    }

    void begin_request(const std::string &question, const std::string &image_path = {})
    {
        if (question.empty() && image_path.empty()) {
            draw_chat(false);
            return;
        }
        const std::string prompt = question.empty() ? ui_locale::text(ui_locale::TextId::DescribeImagePrompt) : question;
        if (_session.id.empty() &&
            !_store.create(_session.model, _session.thinking, &_session)) {
            _status = ui_locale::text(ui_locale::TextId::SessionCreateFailed);
            draw_chat(false);
            return;
        }
        if (!_store.append_message(&_session, "user", prompt, image_path)) {
            _status = ui_locale::text(ui_locale::TextId::QuestionSaveFailed);
            draw_chat(false);
            return;
        }
        _messages.push_back({"user", prompt, image_path});
        _streaming_reply.clear();
        _streaming_active = true;
        _status = ui_locale::text(_session.thinking ? ui_locale::TextId::ThinkingDeeply
                                                    : ui_locale::TextId::Thinking);
        _busy = true;
        _cancel_requested = false;
        _exit_when_done = false;
        _line_scroll = 0;
        draw_chat(false);
        M5.Display.waitDisplay();

        if (!ensure_wifi()) {
            _busy = false;
            _streaming_active = false;
            _status = ui_locale::text(ui_locale::TextId::WifiConnectionFailed);
            draw_chat(false);
            return;
        }

        auto *job = new ChatTaskJob;
        deepseek_config_load(&job->config);
        job->model = _session.model;
        job->thinking = _session.thinking;
        job->messages = _messages;
        job->event_queue = _event_queue;
        job->cancel_requested = &_cancel_requested;
        job->external_stack = true;
        BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
            chat_task, "deepseek_api", 12288, job, 4, &_task,
            0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            ESP_LOGW(TAG, "PSRAM network task stack allocation failed; trying internal RAM");
            job->external_stack = false;
            created = xTaskCreatePinnedToCore(
                chat_task, "deepseek_api", 12288, job, 4, &_task, 0);
        }
        if (created != pdPASS) {
            delete job;
            _busy = false;
            _streaming_active = false;
            _status = ui_locale::text(ui_locale::TextId::NetworkTaskStartFailed);
            draw_chat(false);
        }
    }

    void go_back()
    {
        if (_busy) {
            _cancel_requested = true;
            _exit_when_done = true;
            return;
        }
        if (_view == View::Chat) {
            _view = View::Sessions;
            load_sessions();
            draw_sessions(false);
            return;
        }
        ActivityManager::instance().pop();
    }
};

class DeepSeekSetupGuideActivity final : public Activity {
public:
    void on_create() override
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::DeepSeekSetupRequired));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::DeepSeekKeyNotConfigured),
                                 M5.Display.width() / 2, 350, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::OpenDeepSeekSettingsHint),
                                 M5.Display.width() / 2, 440, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToReturn),
                                 M5.Display.width() / 2, 560, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Tap || event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK || key == APP_KEY_OK) {
            ActivityManager::instance().pop();
            return true;
        }
        return false;
    }
};

}  // namespace

void launch_deepseek()
{
    if (!ensure_wifi_configured_or_show_guide()) return;
    if (!deepseek_config_has_api_key()) {
        ActivityManager::instance().push(new DeepSeekSetupGuideActivity());
        return;
    }
    ActivityManager::instance().push(new DeepSeekActivity());
}
