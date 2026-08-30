#include "activities/DeepSeekActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/SettingActivity.h"
#include "activities/TextInputActivity.h"
#include "deepseek/DeepSeekClient.h"
#include "deepseek/DeepSeekConfig.h"
#include "deepseek/DeepSeekSessionStore.h"
#include "ui/SystemFont.h"

#include <M5Unified.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char *TAG = "DEEPSEEK_UI";
constexpr int HEADER_H = 76;
constexpr int PAD = 20;
constexpr int SESSION_TOP = 100;
constexpr int SESSION_ROW_H = 104;
constexpr int CHAT_TOOLBAR_Y = 88;
constexpr int CHAT_TOOLBAR_H = 66;
constexpr int CHAT_BODY_Y = 170;
constexpr int CHAT_INPUT_H = 86;
constexpr int CHAT_INPUT_Y_MARGIN = 20;

void draw_header(const std::string &title)
{
    int width = M5.Display.width();
    M5.Display.fillRect(0, 0, width, HEADER_H, TFT_WHITE);
    system_font::draw_left(M5.Display, "<", 18, 12, TFT_BLACK);
    system_font::draw_center(M5.Display,
                             system_font::ellipsize(title, width - 150),
                             width / 2, 12, TFT_BLACK);
    M5.Display.drawFastHLine(0, HEADER_H - 1, width, TFT_BLACK);
}

bool header_back_hit(const TouchEvent &event)
{
    return event.type == TouchEventType::Tap && event.x < 110 && event.y < HEADER_H;
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

struct ChatTaskJob {
    DeepSeekConfig config;
    DeepSeekModel model = DeepSeekModel::Flash;
    bool thinking = false;
    std::vector<DeepSeekMessage> messages;
    QueueHandle_t result_queue = nullptr;
    const volatile bool *cancel_requested = nullptr;
};

void chat_task(void *argument)
{
    std::unique_ptr<ChatTaskJob> job(static_cast<ChatTaskJob *>(argument));
    auto *result = new DeepSeekClientResult(
        DeepSeekClient::chat(job->config, job->model, job->thinking, job->messages,
                             job->cancel_requested));
    if (xQueueSend(job->result_queue, &result, pdMS_TO_TICKS(100)) != pdTRUE) {
        delete result;
    }
    vTaskDelete(nullptr);
}

class DeepSeekActivity final : public Activity {
public:
    ~DeepSeekActivity() override
    {
        if (_result_queue) vQueueDelete(_result_queue);
    }

    void on_create() override
    {
        _result_queue = xQueueCreate(1, sizeof(DeepSeekClientResult *));
        load_sessions();
        draw_sessions(true);
    }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        if (!_pending_question.empty() && !_busy) {
            std::string question = std::move(_pending_question);
            _pending_question.clear();
            begin_request(question);
        } else if (_view == View::Chat) {
            draw_chat(false);
        } else {
            load_sessions();
            draw_sessions(false);
        }
    }

    void loop() override
    {
        if (!_busy || !_result_queue) return;
        DeepSeekClientResult *result = nullptr;
        if (xQueueReceive(_result_queue, &result, 0) != pdTRUE) return;
        _busy = false;
        _task = nullptr;
        std::unique_ptr<DeepSeekClientResult> owned(result);
        if (_exit_when_done) {
            ActivityManager::instance().pop();
            return;
        }
        if (result->success) {
            if (_store.append_message(&_session, "assistant", result->content)) {
                _messages.push_back({"assistant", result->content});
                _status.clear();
                _line_scroll = 0;
            } else {
                _status = "Reply received, but saving it failed";
            }
        } else {
            _status = result->canceled ? "Canceled" : result->error;
        }
        draw_chat(false);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            go_back();
            return true;
        }
        if (_view == View::Sessions) return sessions_touch(event);
        return chat_touch(event);
    }

    bool on_key(int key) override
    {
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
        if (_busy) return true;
        if (key == APP_KEY_UP) scroll_chat(6);
        else if (key == APP_KEY_DOWN) scroll_chat(-6);
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
    int _session_focus = 0;
    int _session_scroll = 0;
    int _line_scroll = 0;
    TaskHandle_t _task = nullptr;
    QueueHandle_t _result_queue = nullptr;
    DeepSeekSessionStore _store;
    std::vector<DeepSeekSessionMeta> _sessions;
    DeepSeekSessionMeta _session;
    std::vector<DeepSeekMessage> _messages;
    std::string _pending_question;
    std::string _status;

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
        draw_header("DeepSeek");
        for (int row = 0; row < rows; ++row) {
            int index = _session_scroll + row;
            if (index >= session_row_count()) break;
            int y = SESSION_TOP + row * SESSION_ROW_H;
            bool selected = index == _session_focus;
            uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
            uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRect(PAD, y, width - PAD * 2, SESSION_ROW_H - 4, bg);
            if (index == 0) {
                system_font::draw_left(M5.Display, "+  New Chat", PAD + 16, y + 24, fg);
            } else {
                const auto &session = _sessions[index - 1];
                system_font::draw_left(M5.Display,
                                       system_font::ellipsize(session.title, width - 230),
                                       PAD + 16, y + 8, fg);
                system_font::draw_left(M5.Display, deepseek_model_label(session.model),
                                       PAD + 16, y + 53, fg);
            }
            M5.Display.drawFastHLine(PAD, y + SESSION_ROW_H - 4,
                                     width - PAD * 2, TFT_BLACK);
        }
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
        if (event.type != TouchEventType::Tap || event.y < SESSION_TOP) return false;
        int row = (event.y - SESSION_TOP) / SESSION_ROW_H;
        int index = _session_scroll + row;
        if (index >= 0 && index < session_row_count()) {
            _session_focus = index;
            open_session_row(index);
            return true;
        }
        return false;
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
                _status = "Failed to load conversation history";
                return;
            }
        } else {
            return;
        }
        _view = View::Chat;
        draw_chat(false);
    }

    std::vector<std::string> transcript_lines() const
    {
        std::vector<std::string> lines;
        int max_width = M5.Display.width() - PAD * 2 - 12;
        for (const auto &message : _messages) {
            lines.push_back(message.role == "user" ? "You" : "DeepSeek");
            wrap_text(message.content, max_width, &lines);
            lines.emplace_back();
        }
        if (!_status.empty() && !_busy) {
            lines.push_back("Status");
            wrap_text(_status, max_width, &lines);
        }
        return lines;
    }

    int visible_chat_lines() const
    {
        int bottom = M5.Display.height() - CHAT_INPUT_H - CHAT_INPUT_Y_MARGIN - 18;
        return std::max(1, (bottom - CHAT_BODY_Y) / system_font::line_height());
    }

    void draw_chat(bool quality)
    {
        int width = M5.Display.width();
        int height = M5.Display.height();
        int input_y = height - CHAT_INPUT_H - CHAT_INPUT_Y_MARGIN;
        std::vector<std::string> lines = transcript_lines();
        int visible = visible_chat_lines();
        int max_scroll = std::max(0, static_cast<int>(lines.size()) - visible);
        _line_scroll = std::max(0, std::min(_line_scroll, max_scroll));
        int start = std::max(0, static_cast<int>(lines.size()) - visible - _line_scroll);

        M5.Display.startWrite();
        M5.Display.setDither(false);
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(_session.title);

        constexpr int toolbar_gap = 8;
        int toolbar_width = (width - PAD * 2 - toolbar_gap * 2) / 3;
        const char *toolbar_labels[] = {
            "New Chat",
            deepseek_model_label(_session.model),
            _session.thinking ? "Think On" : "Think Off",
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
            system_font::draw_left(M5.Display, lines[index], PAD + 6, y, TFT_BLACK);
            y += system_font::line_height();
        }

        M5.Display.fillRoundRect(PAD, input_y, width - PAD * 2,
                                 CHAT_INPUT_H, 8, _busy ? TFT_WHITE : TFT_BLACK);
        M5.Display.drawRoundRect(PAD, input_y, width - PAD * 2,
                                 CHAT_INPUT_H, 8, TFT_BLACK);
        const char *input_label = !_busy ? "Ask DeepSeek"
                                  : _session.thinking ? "Thinking deeply... (Back to cancel)"
                                                      : "Thinking... (Back to cancel)";
        system_font::draw_center(M5.Display, input_label,
                                 width / 2,
                                 input_y + std::max(0, (CHAT_INPUT_H - system_font::line_height()) / 2),
                                 _busy ? TFT_BLACK : TFT_WHITE);
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool chat_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::SwipeUp) {
            scroll_chat(-visible_chat_lines() / 2);
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            scroll_chat(visible_chat_lines() / 2);
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (_busy) return true;
        if (event.y >= CHAT_TOOLBAR_Y && event.y < CHAT_TOOLBAR_Y + CHAT_TOOLBAR_H) {
            int screen_width = std::max(1, static_cast<int>(M5.Display.width()));
            int third = std::min(2, event.x * 3 / screen_width);
            if (third == 0) create_new_chat();
            else if (third == 1) toggle_model();
            else toggle_thinking();
            return true;
        }
        int input_y = M5.Display.height() - CHAT_INPUT_H - CHAT_INPUT_Y_MARGIN;
        if (event.y >= input_y) {
            open_input();
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
            _status = "Failed to save the model selection";
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
            _status = "Failed to save the thinking mode";
            draw_chat(false);
        } else {
            draw_chat(false);
        }
    }

    void open_input()
    {
        TextInputOptions options;
        options.title = std::string("Ask ") + deepseek_model_label(_session.model);
        options.submit_label = "Send";
        options.max_bytes = 2048;
        options.start_in_chinese = true;
        launch_text_input(options, [this](const std::string &value) {
            _pending_question = value;
        });
    }

    void begin_request(const std::string &question)
    {
        if (question.empty()) {
            draw_chat(false);
            return;
        }
        if (_session.id.empty() &&
            !_store.create(_session.model, _session.thinking, &_session)) {
            _status = "Failed to create a session on the SD card";
            draw_chat(false);
            return;
        }
        if (!_store.append_message(&_session, "user", question)) {
            _status = "Failed to save the question";
            draw_chat(false);
            return;
        }
        _messages.push_back({"user", question});
        _status = _session.thinking ? "Thinking deeply..." : "Thinking...";
        _busy = true;
        _cancel_requested = false;
        _exit_when_done = false;
        _line_scroll = 0;
        draw_chat(false);
        M5.Display.waitDisplay();

        auto *job = new ChatTaskJob;
        deepseek_config_load(&job->config);
        job->model = _session.model;
        job->thinking = _session.thinking;
        job->messages = _messages;
        job->result_queue = _result_queue;
        job->cancel_requested = &_cancel_requested;
        BaseType_t created = xTaskCreate(chat_task, "deepseek_api", 12288, job, 4, &_task);
        if (created != pdPASS) {
            delete job;
            _busy = false;
            _status = "Failed to start the network task";
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
        draw_header("DeepSeek Setup Required");
        system_font::draw_center(M5.Display, "DeepSeek API key is not configured",
                                 M5.Display.width() / 2, 350, TFT_BLACK);
        system_font::draw_center(M5.Display, "Open Settings > DeepSeek first",
                                 M5.Display.width() / 2, 440, TFT_BLACK);
        system_font::draw_center(M5.Display, "Tap to return",
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
