#include "activities/RadioActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/SettingActivity.h"
#include "audio/audio_radio.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "ui/icon_bitmap_1bpp.h"
#include "ui/music_icons_1bpp.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

static const char *TAG = "RADIO";
// HLS creates its nested HTTP reader at priority 10 on CPU0.  app_main is
// normally priority 1, so once live HLS traffic starts it may never get a turn
// to poll touch or run this Activity.  Match (rather than exceed) HTTP so the
// FreeRTOS time slice keeps UI responsive while USB's priority-20 service task
// remains unconditionally above both.
static constexpr UBaseType_t RADIO_UI_TASK_PRIORITY = 10;

static constexpr int DESIGN_W = 684;
static constexpr int DESIGN_H = 1216;
static constexpr int TITLE_X = 73;
static constexpr int TITLE_Y = 74;
static constexpr int PANEL_X = 85;
static constexpr int PANEL_Y = 158;
static constexpr int PANEL_W = 515;
static constexpr int PANEL_H = 708;
static constexpr int MAIN_CONTROL_Y = 996;
static constexpr int BOTTOM_CONTROL_Y = 1135;
static constexpr int LIST_ROW_H = 62;
static constexpr int LIST_INSET = 8;
static constexpr int LIST_FOOTER_H = 42;
static constexpr int64_t SWITCH_STATUS_COOLDOWN_US = 1000000;

static int design_x(int value)
{
    return static_cast<int>((static_cast<int64_t>(value) * M5.Display.width()) / DESIGN_W);
}

static int design_y(int value)
{
    return static_cast<int>((static_cast<int64_t>(value) * M5.Display.height()) / DESIGN_H);
}

struct RadioStation {
    const char *name;
    const char *url;
};

static const RadioStation STATIONS[] = {
    {"中国之声", "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
    {"经济之声", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8"},
    {"文艺之声", "http://ngcdn001.cnr.cn/live/wyzs/index.m3u8"},
    {"台海之声", "http://ngcdn002.cnr.cn/live/zhzs/index.m3u8"},
    {"乡村之声", "http://ngcdn002.cnr.cn/live/xczs/index.m3u8"},
    {"老年之声", "http://ngcdn002.cnr.cn/live/lnzs/index.m3u8"},
    {"民族之声", "http://ngcdn002.cnr.cn/live/mzzs/index.m3u8"},
    {"香港之声", "http://ngcdn002.cnr.cn/live/xgzs/index.m3u8"},
    {"大湾区之声", "http://ngcdn002.cnr.cn/live/hxzs/index.m3u8"},
    {"神州之声", "http://ngcdn002.cnr.cn/live/szzs/index.m3u8"},
    {"中国交通广播", "http://ngcdn002.cnr.cn/live/gsgljtgb/index.m3u8"},
    {"音乐之声", "http://ngcdn002.cnr.cn/live/yyzs/index.m3u8"},
    {"经典音乐广播", "http://ngcdn002.cnr.cn/live/dszs/index.m3u8"},
    {"上海流行音乐","http://lhttp.qingting.fm/live/273/64k.mp3"},
    {"江苏经典流行音乐","http://lhttp.qingting.fm/live/4938/64k.mp3"},
    {"怀旧好声音","http://lhttp.qingting.fm/live/1223/64k.mp3"},
    {"954汽车音乐广播","http://lhttp.qingting.fm/live/1936/64k.mp3"},
    {"亚洲音乐台","http://lhttp.qingting.fm/live/5022405/64k.mp3"},
};

static const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);

struct RadioPlayResult {
    int index;
    uint32_t request_id;
    esp_err_t result;
};

struct RadioPlayJob {
    QueueHandle_t result_queue;
    int index;
    uint32_t request_id;
    const char *url;
};

static void radio_play_worker(void *arg)
{
    RadioPlayJob *job = static_cast<RadioPlayJob *>(arg);
    ESP_LOGI(TAG, "Radio play worker begin station=%d", job->index);
    RadioPlayResult result = {
        .index = job->index,
        .request_id = job->request_id,
        .result = audio_radio_play(job->url),
    };
    ESP_LOGI(TAG, "Radio play worker complete station=%d request=%lu result=%s",
             result.index, (unsigned long)result.request_id,
             esp_err_to_name(result.result));
    (void)xQueueSend(job->result_queue, &result, portMAX_DELAY);
    delete job;
    vTaskDelete(nullptr);
}

class RadioActivity final : public Activity {
public:
    ~RadioActivity() override
    {
        if (_play_task != nullptr) {
            // Normal Back handling defers the pop until the worker reports.
            // Keep this bounded guard for exceptional Activity teardown; the
            // worker owns no Activity pointer, so leaking the tiny queue is
            // safer than deleting an object it may still send to.
            RadioPlayResult ignored = {};
            if (xQueueReceive(_play_results, &ignored, pdMS_TO_TICKS(1000)) == pdTRUE) {
                _play_task = nullptr;
            }
        }
        if (_play_task == nullptr && _play_results != nullptr) {
            vQueueDelete(_play_results);
            _play_results = nullptr;
        } else if (_play_task != nullptr) {
            ESP_LOGW(TAG, "Radio play worker still active during teardown; preserving result queue");
            // Do not deinitialize audio or Wi-Fi underneath the worker. Normal
            // navigation never reaches this branch because request_leave()
            // waits for the result before popping the Activity.
            restore_ui_priority();
            return;
        }
        audio_radio_deinit();
        // Keep the connection only when the user selected Always On.
        wifi_setting_release();
        restore_ui_priority();
    }

    void on_create() override
    {
        // app_main normally runs at priority 1. The HLS component's nested HTTP
        // reader is fixed at priority 10 on CPU0; use the same priority so
        // FreeRTOS time slicing lets touch/UI run during a live stream.
        _ui_task = xTaskGetCurrentTaskHandle();
        _ui_original_priority = uxTaskPriorityGet(_ui_task);
        if (_ui_original_priority < RADIO_UI_TASK_PRIORITY) {
            vTaskPrioritySet(_ui_task, RADIO_UI_TASK_PRIORITY);
        }
        _play_results = xQueueCreate(2, sizeof(RadioPlayResult));
        draw_message(ui_locale::text(ui_locale::TextId::LoadingWifi));
        _wifi_ok = connect_wifi();
        if (!_wifi_ok) {
            _status = ui_locale::text(ui_locale::TextId::ConfigureWifiHint);
        }
        draw_all();
    }

    void loop() override
    {
        service_play_result();
        if (_leave_pending && _play_task == nullptr) {
            ActivityManager::instance().pop();
            return;
        }
        audio_playback_state_t state = audio_radio_get_state();
        if (_switching && _play_task == nullptr &&
            _current == _switch_target &&
            esp_timer_get_time() - _switch_hint_finished_us >= SWITCH_STATUS_COOLDOWN_US) {
            if (audio_radio_is_playback_ready()) {
                _switching = false;
                _status.clear();
                draw_all(epd_mode_t::epd_fast);
                return;
            }
            if (state == AUDIO_PLAYBACK_ERROR) {
                _switching = false;
                _error_drawn = true;
                if (_status == ui_locale::text(ui_locale::TextId::SwitchingStation) ||
                    _status == ui_locale::text(ui_locale::TextId::StartingStation)) {
                    _status = ui_locale::text(ui_locale::TextId::PlaybackFailed);
                }
                draw_all(epd_mode_t::epd_fast);
                return;
            }
        }
        if (_switching) return;
        // Process the completed station transition first, so USB maintenance
        // can never leave the screen stuck on "Starting station...". The
        // shared service is rate-limited and uses a lock-free connection
        // snapshot, so this call cannot wait behind PCM writes.
        if (_play_task == nullptr) {
            audio_radio_service_output();
        }
        if (state == AUDIO_PLAYBACK_ERROR && !_error_drawn) {
            _error_drawn = true;
            _status = ui_locale::text(ui_locale::TextId::PlaybackFailed);
            draw_all();
        } else if (state != AUDIO_PLAYBACK_ERROR) {
            _error_drawn = false;
        }
        update_state_if_changed();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            request_leave();
            return true;
        }
        if (event.type == TouchEventType::SwipeUp) {
            if (_show_list) page_down();
            return _show_list;
        }
        if (event.type == TouchEventType::SwipeDown) {
            if (_show_list) page_up();
            return _show_list;
        }
        if (event.type != TouchEventType::Tap) return false;

        // Keep the back touch target while leaving the arrow visually hidden.
        if (event.x < design_x(100) && event.y < design_y(130)) {
            request_leave();
            return true;
        }
        if (event.x >= design_x(558) && event.y < design_y(130)) {
            cycle_audio_output();
            return true;
        }
        if (hit_center(event.x, event.y, 207, MAIN_CONTROL_Y, 62, 55)) {
            previous_station();
            return true;
        }
        if (hit_center(event.x, event.y, 342, MAIN_CONTROL_Y, 62, 55)) {
            toggle_pause();
            return true;
        }
        if (hit_center(event.x, event.y, 479, MAIN_CONTROL_Y, 62, 55)) {
            next_station();
            return true;
        }
        if (hit_center(event.x, event.y, 91, BOTTOM_CONTROL_Y, 52, 46)) {
            random_station();
            return true;
        }
        if (hit_center(event.x, event.y, 207, BOTTOM_CONTROL_Y, 52, 46)) {
            next_station();
            return true;
        }
        if (hit_center(event.x, event.y, 342, BOTTOM_CONTROL_Y, 52, 46)) {
            _show_list = !_show_list;
            ensure_focus_visible();
            draw_all();
            return true;
        }
        if (hit_center(event.x, event.y, 474, BOTTOM_CONTROL_Y, 52, 46)) {
            adjust_volume(-10);
            return true;
        }
        if (hit_center(event.x, event.y, 579, BOTTOM_CONTROL_Y, 52, 46)) {
            adjust_volume(10);
            return true;
        }

        int index = hit_station(event.x, event.y);
        if (index >= 0) {
            _focus = index;
            if (_current == index) toggle_pause();
            else play_station(index);
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            request_leave();
            return true;
        }
        if (key == APP_KEY_EXTRA) {
            random_station();
            return true;
        }
        if (key == APP_KEY_UP && _focus > 0) {
            _show_list = true;
            --_focus;
            ensure_focus_visible();
            draw_all();
            return true;
        }
        if (key == APP_KEY_DOWN && _focus + 1 < STATION_COUNT) {
            _show_list = true;
            ++_focus;
            ensure_focus_visible();
            draw_all();
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_show_list && _current != _focus) play_station(_focus);
            else toggle_pause();
            return true;
        }
        return false;
    }

private:
    bool _wifi_ok = false;
    bool _show_list = true;
    bool _error_drawn = false;
    bool _switching = false;
    int _focus = 0;
    int _scroll = 0;
    int _current = -1;
    int _switch_target = -1;
    int64_t _switch_hint_finished_us = 0;
    QueueHandle_t _play_results = nullptr;
    TaskHandle_t _play_task = nullptr;
    int _queued_station = -1;
    uint32_t _next_request_id = 0;
    uint32_t _active_request_id = 0;
    uint32_t _queued_request_id = 0;
    bool _leave_pending = false;
    std::string _status;
    // State the panel and control row were last drawn with. Losing or regaining
    // the USB output pauses and resumes playback with no touch behind it, so the
    // two places showing the state have to notice that on their own.
    audio_playback_state_t _drawn_state = AUDIO_PLAYBACK_IDLE;
    TaskHandle_t _ui_task = nullptr;
    UBaseType_t _ui_original_priority = 0;

    void restore_ui_priority()
    {
        if (_ui_task != nullptr && uxTaskPriorityGet(_ui_task) != _ui_original_priority) {
            vTaskPrioritySet(_ui_task, _ui_original_priority);
        }
        _ui_task = nullptr;
    }

    bool hit_center(int x, int y, int center_x, int center_y, int half_w, int half_h) const
    {
        return x >= design_x(center_x - half_w) && x <= design_x(center_x + half_w) &&
               y >= design_y(center_y - half_h) && y <= design_y(center_y + half_h);
    }

    int list_top() const
    {
        return design_y(PANEL_Y + LIST_INSET);
    }

    int list_row_height() const
    {
        return std::max(1, design_y(LIST_ROW_H));
    }

    int visible_rows() const
    {
        int usable = design_y(PANEL_H - LIST_INSET * 2 - LIST_FOOTER_H);
        return std::max(1, usable / list_row_height());
    }

    void ensure_focus_visible()
    {
        int rows = visible_rows();
        if (_focus < _scroll) _scroll = _focus;
        if (_focus >= _scroll + rows) _scroll = _focus - rows + 1;
        _scroll = std::max(0, std::min(_scroll, std::max(0, STATION_COUNT - rows)));
    }

    int hit_station(int x, int y) const
    {
        if (!_show_list || x < design_x(PANEL_X) || x > design_x(PANEL_X + PANEL_W)) return -1;
        if (y < list_top()) return -1;
        int row = (y - list_top()) / list_row_height();
        if (row < 0 || row >= visible_rows()) return -1;
        int index = _scroll + row;
        return index >= 0 && index < STATION_COUNT ? index : -1;
    }

    void page_up()
    {
        int rows = visible_rows();
        _focus = std::max(0, _focus - rows);
        _scroll = std::max(0, _scroll - rows);
        draw_all();
    }

    void page_down()
    {
        int rows = visible_rows();
        _focus = std::min(STATION_COUNT - 1, _focus + rows);
        _scroll = std::min(std::max(0, STATION_COUNT - rows), _scroll + rows);
        draw_all();
    }

    bool connect_wifi()
    {
        esp_err_t ret = wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(ret));
            return false;
        }
        return true;
    }

    const char *state_text() const
    {
        if (!_status.empty()) return _status.c_str();
        switch (audio_radio_get_state()) {
            case AUDIO_PLAYBACK_PLAYING: return ui_locale::text(ui_locale::TextId::Playing);
            case AUDIO_PLAYBACK_PAUSED: return ui_locale::text(ui_locale::TextId::Paused);
            case AUDIO_PLAYBACK_ERROR: return ui_locale::text(ui_locale::TextId::PlaybackFailed);
            case AUDIO_PLAYBACK_STOPPED: return ui_locale::text(ui_locale::TextId::Stopped);
            default: return ui_locale::text(_wifi_ok ? ui_locale::TextId::SelectStation
                                                      : ui_locale::TextId::WifiUnavailable);
        }
    }

    void play_station(int index)
    {
        if (!_wifi_ok || index < 0 || index >= STATION_COUNT) return;
        const uint32_t request_id = ++_next_request_id;
        _focus = index;
        ensure_focus_visible();
        _switching = true;
        _switch_target = index;
        _status = ui_locale::text(_current >= 0 ? ui_locale::TextId::SwitchingStation
                                                 : ui_locale::TextId::StartingStation);
        draw_switching_hint(index);
        if (_play_task != nullptr) {
            // Only the newest queued selection matters. The active worker owns
            // the physical pipeline until it reports; after that, immediately
            // move the pipeline to this latest requested station.
            _queued_station = index;
            _queued_request_id = request_id;
            return;
        }
        start_play_worker(index, request_id);
    }

    void start_play_worker(int index, uint32_t request_id)
    {
        if (_play_results == nullptr) {
            _status = ui_locale::text(ui_locale::TextId::RadioWorkerUnavailable);
            _switching = false;
            return;
        }
        RadioPlayJob *job = new (std::nothrow) RadioPlayJob{
            _play_results, index, request_id, STATIONS[index].url};
        if (job == nullptr ||
            xTaskCreatePinnedToCoreWithCaps(radio_play_worker, "radio_play", 8 * 1024,
                                            job, 11, &_play_task, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            delete job;
            _play_task = nullptr;
            _status = ui_locale::text(ui_locale::TextId::RadioWorkerUnavailable);
            _switching = false;
            draw_all();
            return;
        }
        _active_request_id = request_id;
    }

    void service_play_result()
    {
        if (_play_task == nullptr || _play_results == nullptr) return;
        RadioPlayResult result = {};
        if (xQueueReceive(_play_results, &result, 0) != pdTRUE) return;
        ESP_LOGI(TAG, "Radio UI received play result station=%d request=%lu latest=%lu result=%s",
                 result.index, (unsigned long)result.request_id,
                 (unsigned long)_next_request_id, esp_err_to_name(result.result));
        _play_task = nullptr;
        _active_request_id = 0;
        const bool superseded = result.request_id != _next_request_id;
        if (result.result == ESP_OK) {
            // This is the station the physical pipeline now owns, even when a
            // newer request is waiting. Keep it as current truth until that
            // newer worker actually completes.
            _current = result.index;
            _show_list = false;
            ESP_LOGI(TAG, "Playing station %d/%d: %s", result.index + 1,
                     STATION_COUNT, STATIONS[result.index].name);
        } else if (!superseded) {
            char message[96] = {};
            snprintf(message, sizeof(message), "%s%s",
                     ui_locale::text(ui_locale::TextId::PlayFailedPrefix),
                     esp_err_to_name(result.result));
            _status = message;
            _switching = false;
            ESP_LOGE(TAG, "Failed to play station %s: %s",
                     STATIONS[result.index].name, esp_err_to_name(result.result));
            draw_all();
        }
        if (!_leave_pending && _queued_station >= 0) {
            int queued = _queued_station;
            uint32_t queued_request = _queued_request_id;
            _queued_station = -1;
            _queued_request_id = 0;
            // Do not call play_station(): that would allocate another request
            // id and redraw the same hint. This queued id already represents
            // the user's latest selection.
            start_play_worker(queued, queued_request);
            return;
        }
        if (superseded) {
            // Defensive fallback for a result whose replacement was cancelled
            // during teardown. It must never clear the latest switching state.
            return;
        }
    }

    void request_leave()
    {
        if (_play_task == nullptr) {
            ActivityManager::instance().pop();
            return;
        }
        _leave_pending = true;
        _queued_station = -1;
        _queued_request_id = 0;
        _status = ui_locale::text(ui_locale::TextId::FinishingAudioStartup);
        draw_station_status_card(_switch_target >= 0 ? _switch_target : _focus,
                                 _status.c_str());
    }

    void draw_switching_hint(int index)
    {
        draw_station_status_card(index, _status.c_str());
        _switch_hint_finished_us = esp_timer_get_time();
    }

    void draw_station_status_card(int index, const char *status)
    {
        const int left = design_x(PANEL_X) + 1;
        const int top = design_y(PANEL_Y) + 1;
        const int width = std::max(1, design_x(PANEL_W) - 2);
        const int height = std::max(1, design_y(PANEL_H) - 2);
        const int center_x = design_x(PANEL_X + PANEL_W / 2);

        // Finish any preceding page update so the hint is physically visible
        // before the blocking network switch begins.
        M5.Display.waitDisplay();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        // Replace the old list/now-playing contents instead of drawing a
        // floating card over them, which leaves stale text underneath.
        M5.Display.fillRect(left, top, width, height, TFT_WHITE);
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(STATIONS[index].name,
                                                        design_x(PANEL_W - 60)),
                                 center_x, design_y(PANEL_Y + PANEL_H / 2 - 70), TFT_BLACK);
        system_font::draw_center(M5.Display, status,
                                 center_x, design_y(PANEL_Y + PANEL_H / 2), TFT_BLACK);
        char details[64] = {};
        snprintf(details, sizeof(details), ui_locale::text(ui_locale::TextId::VolumeFormat),
                 audio_radio_get_volume());
        system_font::draw_center(M5.Display, details, center_x,
                                 design_y(PANEL_Y + PANEL_H / 2 + 70), TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
        M5.Display.waitDisplay();
    }

    void next_station()
    {
        int next = _current >= 0 ? (_current + 1) % STATION_COUNT : _focus;
        play_station(next);
    }

    void previous_station()
    {
        int previous = _current >= 0
                           ? (_current - 1 + STATION_COUNT) % STATION_COUNT
                           : _focus;
        play_station(previous);
    }

    void random_station()
    {
        int index = (int)(esp_random() % (uint32_t)STATION_COUNT);
        if (STATION_COUNT > 1 && index == _current) {
            index = (index + 1 + (int)(esp_random() % (uint32_t)(STATION_COUNT - 1))) %
                    STATION_COUNT;
        }
        play_station(index);
    }

    void toggle_pause()
    {
        if (!_wifi_ok) return;
        if (_switching) return;
        if (_current < 0) {
            play_station(_focus);
            return;
        }

        audio_playback_state_t state = audio_radio_get_state();
        esp_err_t ret = ESP_OK;
        if (state == AUDIO_PLAYBACK_PLAYING) {
            ret = audio_radio_pause();
        } else if (state == AUDIO_PLAYBACK_PAUSED) {
            ret = audio_radio_resume();
        } else {
            play_station(_current);
            return;
        }
        if (ret == ESP_OK) {
            _status.clear();
        } else {
            char message[96] = {};
            snprintf(message, sizeof(message), "%s%s",
                     ui_locale::text(ui_locale::TextId::ControlFailedPrefix), esp_err_to_name(ret));
            _status = message;
        }
        draw_all();
    }

    void adjust_volume(int delta)
    {
        int volume = std::max(0, std::min(100, audio_radio_get_volume() + delta));
        esp_err_t ret = audio_radio_set_volume(volume);
        if (ret == ESP_OK) {
            // Volume already has its own field in the details row. Keep the
            // center line reserved for the playback state (Playing/Paused).
            _status.clear();
        } else {
            char message[64] = {};
            snprintf(message, sizeof(message), "%s%s",
                     ui_locale::text(ui_locale::TextId::VolumeFailedPrefix), esp_err_to_name(ret));
            _status = message;
        }
        draw_all();
    }

    void draw_message(const char *message)
    {
        M5.Display.startWrite();
        // This is the first refresh after entering Radio. Use a full quality
        // waveform to clear remnants from whichever activity was visible.
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::NetworkRadio),
                               design_x(TITLE_X), design_y(TITLE_Y), TFT_BLACK);
        system_font::draw_center(M5.Display, message, M5.Display.width() / 2,
                                 M5.Display.height() / 2 - 20, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_icon(MusicIconId id, int target_x, int target_y, bool active = false)
    {
        const IconBitmap1bpp *icon = music_icons::get(id);
        if (icon == nullptr) return;

        uint16_t foreground = TFT_BLACK;
        uint16_t background = TFT_WHITE;
        if (active) {
            int center_x = design_x(target_x);
            int center_y = design_y(target_y);
            int width = design_x(62);
            int height = design_y(58);
            M5.Display.fillRoundRect(center_x - width / 2, center_y - height / 2,
                                     width, height, std::max(4, design_x(8)), TFT_BLACK);
            foreground = TFT_WHITE;
            background = TFT_BLACK;
        }

        draw_icon_1bpp(M5.Display, *icon, design_x(target_x), design_y(target_y),
                       foreground, background, std::max(1, design_x(icon->width)),
                       std::max(1, design_y(icon->height)));
    }

    void draw_station_list()
    {
        int left = design_x(PANEL_X + LIST_INSET);
        int width = design_x(PANEL_W - LIST_INSET * 2);
        int top = list_top();
        int row_height = list_row_height();
        int rows = visible_rows();

        for (int row = 0; row < rows; ++row) {
            int index = _scroll + row;
            if (index >= STATION_COUNT) break;
            int y = top + row * row_height;
            bool selected = index == _focus;
            uint16_t background = selected ? TFT_BLACK : TFT_WHITE;
            uint16_t foreground = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRect(left, y + 2, width, row_height - 4, background);
            if (index == _current) {
                M5.Display.fillRect(left + design_x(5), y + design_y(10),
                                    std::max(2, design_x(4)),
                                    row_height - design_y(20), foreground);
            }
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(STATIONS[index].name,
                                                          width - design_x(28)),
                                   left + design_x(18), y + design_y(10), foreground);
        }

        char position[32] = {};
        snprintf(position, sizeof(position), "%d / %d", _focus + 1, STATION_COUNT);
        system_font::draw_center(M5.Display, position,
                                 design_x(PANEL_X + PANEL_W / 2),
                                 design_y(PANEL_Y + PANEL_H - 38), TFT_BLACK);
    }

    void draw_now_playing_panel()
    {
        if (_show_list) {
            draw_station_list();
            return;
        }

        int center_x = design_x(PANEL_X + PANEL_W / 2);
        const char *station = _current >= 0 ? STATIONS[_current].name
                                            : ui_locale::text(ui_locale::TextId::NoStationSelected);
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(station, design_x(PANEL_W - 60)),
                                 center_x, design_y(PANEL_Y + PANEL_H / 2 - 70), TFT_BLACK);
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(state_text(), design_x(PANEL_W - 60)),
                                 center_x, design_y(PANEL_Y + PANEL_H / 2), TFT_BLACK);

        char details[64] = {};
        if (_current >= 0) {
            snprintf(details, sizeof(details), ui_locale::text(ui_locale::TextId::VolumeFormat),
                     audio_radio_get_volume());
        } else {
            snprintf(details, sizeof(details), ui_locale::text(ui_locale::TextId::StationCountFormat),
                     STATION_COUNT);
        }
        system_font::draw_center(M5.Display, details, center_x,
                                 design_y(PANEL_Y + PANEL_H / 2 + 70), TFT_BLACK);
    }

    bool update_state_if_changed()
    {
        if (audio_radio_get_state() == _drawn_state) return false;
        // Level-triggered on purpose: a refresh already in flight only means the
        // next poll does the work, so nothing is lost by waiting for the panel.
        if (M5.Display.displayBusy()) return false;

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        if (!_show_list) {
            // Both the state line and the play/pause glyph are read out of the
            // playback state. The line is centred, so unlike the glyph it cannot
            // be drawn over in place - "Playing" is wider than "Paused" - so put
            // the panel back the way draw_all() leaves it. Skipped for the station
            // list, which shows no state and is far too large to refresh here.
            M5.Display.fillRect(design_x(PANEL_X) + 1, design_y(PANEL_Y) + 1,
                                std::max(1, design_x(PANEL_W) - 2),
                                std::max(1, design_y(PANEL_H) - 2), TFT_WHITE);
            draw_now_playing_panel();
        }
        draw_controls();
        M5.Display.display();
        M5.Display.endWrite();
        return true;
    }

    // The two glyphs this slot alternates between are not the same size - Play is
    // 40x50, Pause 28x36 - and draw_icon_1bpp paints only the bitmap's own extent,
    // with no fixed cell behind it. Drawing the smaller one over the larger
    // therefore leaves a ring of the old glyph. draw_all() hides that behind its
    // fillScreen; a partial repaint has to clear the union of the two first.
    void draw_play_pause_icon()
    {
        const IconBitmap1bpp *play = music_icons::get(MusicIconId::Play);
        const IconBitmap1bpp *pause = music_icons::get(MusicIconId::Pause);
        if (play != nullptr && pause != nullptr) {
            const int width = design_x(std::max(play->width, pause->width));
            const int height = design_y(std::max(play->height, pause->height));
            M5.Display.fillRect(design_x(342) - width / 2, design_y(MAIN_CONTROL_Y) - height / 2,
                                width, height, TFT_WHITE);
        }
        draw_icon(audio_radio_get_state() == AUDIO_PLAYBACK_PLAYING ? MusicIconId::Pause
                                                                   : MusicIconId::Play,
                  342, MAIN_CONTROL_Y);
    }

    void draw_controls()
    {
        audio_playback_state_t state = audio_radio_get_state();
        _drawn_state = state;
        draw_icon(MusicIconId::Previous, 207, MAIN_CONTROL_Y);
        draw_play_pause_icon();
        draw_icon(MusicIconId::Next, 479, MAIN_CONTROL_Y);

        // On radio these two icons are actions, not persistent play modes.
        draw_icon(MusicIconId::Shuffle, 91, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::Sequence, 207, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::Playlist, 342, BOTTOM_CONTROL_Y, _show_list);
        draw_icon(MusicIconId::VolumeDown, 474, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::VolumeUp, 579, BOTTOM_CONTROL_Y);
    }

    void draw_audio_output_selector()
    {
        const int x = design_x(570);
        const int y = design_y(34);
        const int w = design_x(88);
        const int h = design_y(56);
        M5.Display.drawRect(x, y, w, h, TFT_BLACK);
        system_font::draw_center(M5.Display, audio_radio_get_output_mode_name(),
                                 x + w / 2, y + design_y(13), TFT_BLACK);
    }

    void cycle_audio_output()
    {
        audio_output_mode_t current = audio_radio_get_output_mode();
        audio_output_mode_t next = current == AUDIO_OUTPUT_SPEAKER
                                       ? AUDIO_OUTPUT_USB
                                       : (current == AUDIO_OUTPUT_USB
                                              ? AUDIO_OUTPUT_AUTO
                                              : AUDIO_OUTPUT_SPEAKER);
        esp_err_t ret = audio_radio_set_output_mode(next);
        if (ret != ESP_OK) {
            _status = ui_locale::text(ui_locale::TextId::UsbHeadsetUnavailable);
            ESP_LOGW(TAG, "Audio output switch failed: %s", esp_err_to_name(ret));
        } else {
            _status.clear();
        }
        draw_all();
    }

    void draw_all(epd_mode_t mode = epd_mode_t::epd_fast)
    {
        ensure_focus_visible();
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
        M5.Display.fillScreen(TFT_WHITE);

        std::string title;
        if (_show_list) title = ui_locale::text(ui_locale::TextId::RadioStations);
        else if (_current >= 0) title = STATIONS[_current].name;
        else title = ui_locale::text(ui_locale::TextId::NetworkRadio);
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(title, design_x(455)),
                               design_x(TITLE_X), design_y(TITLE_Y), TFT_BLACK);
        draw_audio_output_selector();

        M5.Display.drawRect(design_x(PANEL_X), design_y(PANEL_Y),
                            design_x(PANEL_W), design_y(PANEL_H), TFT_BLACK);
        draw_now_playing_panel();
        draw_controls();

        M5.Display.display();
        M5.Display.endWrite();
    }
};

void launch_radio()
{
    if (!ensure_wifi_configured_or_show_guide()) return;
    ActivityManager::instance().push(new RadioActivity());
}
