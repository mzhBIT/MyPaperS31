#include "activities/RadioActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "audio/audio_radio.h"
#include "ui/SystemFont.h"
#include "ui/icon_bitmap_1bpp.h"
#include "ui/music_icons_1bpp.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

static const char *TAG = "RADIO";
static const char *WIFI_CONFIG_PATH = "/sdcard/wifi.txt";

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

static std::string trim_copy(const std::string &s)
{
    size_t start = 0;
    while (start < s.size() && (unsigned char)s[start] <= ' ') start++;
    size_t end = s.size();
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;
    return s.substr(start, end - start);
}

static bool load_wifi_config(std::string *ssid, std::string *password)
{
    if (!ssid || !password) return false;

    FILE *fp = std::fopen(WIFI_CONFIG_PATH, "rb");
    if (!fp) return false;

    char line[160] = {};
    std::string first_line;
    std::string second_line;
    while (std::fgets(line, sizeof(line), fp)) {
        std::string s = trim_copy(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq != std::string::npos) {
            std::string key = trim_copy(s.substr(0, eq));
            std::string val = trim_copy(s.substr(eq + 1));
            if (key == "ssid") *ssid = val;
            else if (key == "password" || key == "pass") *password = val;
        } else if (first_line.empty()) {
            first_line = s;
        } else if (second_line.empty()) {
            second_line = s;
        }
    }
    std::fclose(fp);

    if (ssid->empty() && !first_line.empty()) *ssid = first_line;
    if (password->empty() && !second_line.empty()) *password = second_line;
    return !ssid->empty();
}

class RadioActivity final : public Activity {
public:
    ~RadioActivity() override
    {
        audio_radio_deinit();
        // Network features are on-demand; power down the C6 when leaving.
        wifi_setting_deinit(true);
    }

    void on_create() override
    {
        draw_message("Connecting WiFi...");
        _wifi_ok = connect_wifi();
        if (!_wifi_ok) {
            _status = "Create /sdcard/wifi.txt";
        }
        draw_all();
    }

    void loop() override
    {
        audio_playback_state_t state = audio_radio_get_state();
        if (_switching &&
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
                if (_status == "Switching station..." || _status == "Starting station...") {
                    _status = "Radio playback failed";
                }
                draw_all(epd_mode_t::epd_fast);
                return;
            }
        }
        if (_switching) return;
        if (state == AUDIO_PLAYBACK_ERROR && !_error_drawn) {
            _error_drawn = true;
            _status = "Radio playback failed";
            draw_all();
        } else if (state != AUDIO_PLAYBACK_ERROR) {
            _error_drawn = false;
        }
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
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
            ActivityManager::instance().pop();
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
            ActivityManager::instance().pop();
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
    std::string _status;

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
        std::string ssid = CONFIG_WIFI_SETTING_STA_SSID;
        std::string password = CONFIG_WIFI_SETTING_STA_PASSWORD;
        load_wifi_config(&ssid, &password);
        if (ssid.empty()) {
            ESP_LOGW(TAG, "Wi-Fi SSID is empty");
            return false;
        }
        esp_err_t ret = wifi_setting_connect(ssid.c_str(), password.c_str(),
                                             CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
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
            case AUDIO_PLAYBACK_PLAYING: return "Playing";
            case AUDIO_PLAYBACK_PAUSED: return "Paused";
            case AUDIO_PLAYBACK_ERROR: return "Playback failed";
            case AUDIO_PLAYBACK_STOPPED: return "Stopped";
            default: return _wifi_ok ? "Select a station" : "WiFi unavailable";
        }
    }

    void play_station(int index)
    {
        if (!_wifi_ok || index < 0 || index >= STATION_COUNT) return;
        _focus = index;
        ensure_focus_visible();
        _switching = true;
        _switch_target = index;
        _status = _current >= 0 ? "Switching station..." : "Starting station...";

        // Stop the old HLS stream before refreshing the hint. Besides making
        // the state truthful, this keeps active network traffic away from the
        // EPD update used for the switching feedback.
        if (_current >= 0) {
            esp_err_t stop_ret = audio_radio_stop();
            if (stop_ret != ESP_OK) {
                char message[96] = {};
                snprintf(message, sizeof(message), "Switch failed: %s",
                         esp_err_to_name(stop_ret));
                _status = message;
                _switching = false;
                draw_all();
                return;
            }
        }
        draw_switching_hint(index);

        esp_err_t ret = audio_radio_play(STATIONS[index].url);
        if (ret == ESP_OK) {
            _current = index;
            _show_list = false;
            ESP_LOGI(TAG, "Playing station %d/%d: %s", index + 1, STATION_COUNT,
                     STATIONS[index].name);
        } else {
            char message[96] = {};
            snprintf(message, sizeof(message), "Play failed: %s", esp_err_to_name(ret));
            _status = message;
            ESP_LOGE(TAG, "Failed to play station %s: %s", STATIONS[index].name,
                     esp_err_to_name(ret));
        }
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
        snprintf(details, sizeof(details), "%d / %d    Volume %d%%",
                 index + 1, STATION_COUNT, audio_radio_get_volume());
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
            snprintf(message, sizeof(message), "Control failed: %s", esp_err_to_name(ret));
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
            snprintf(message, sizeof(message), "Volume failed: %s", esp_err_to_name(ret));
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
        system_font::draw_left(M5.Display, "Network Radio",
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
        const char *station = _current >= 0 ? STATIONS[_current].name : "No station selected";
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(station, design_x(PANEL_W - 60)),
                                 center_x, design_y(PANEL_Y + PANEL_H / 2 - 70), TFT_BLACK);
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(state_text(), design_x(PANEL_W - 60)),
                                 center_x, design_y(PANEL_Y + PANEL_H / 2), TFT_BLACK);

        char details[64] = {};
        if (_current >= 0) {
            snprintf(details, sizeof(details), "%d / %d    Volume %d%%",
                     _current + 1, STATION_COUNT, audio_radio_get_volume());
        } else {
            snprintf(details, sizeof(details), "%d stations", STATION_COUNT);
        }
        system_font::draw_center(M5.Display, details, center_x,
                                 design_y(PANEL_Y + PANEL_H / 2 + 70), TFT_BLACK);
    }

    void draw_controls()
    {
        audio_playback_state_t state = audio_radio_get_state();
        draw_icon(MusicIconId::Previous, 207, MAIN_CONTROL_Y);
        draw_icon(state == AUDIO_PLAYBACK_PLAYING ? MusicIconId::Pause : MusicIconId::Play,
                  342, MAIN_CONTROL_Y);
        draw_icon(MusicIconId::Next, 479, MAIN_CONTROL_Y);

        // On radio these two icons are actions, not persistent play modes.
        draw_icon(MusicIconId::Shuffle, 91, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::Sequence, 207, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::Playlist, 342, BOTTOM_CONTROL_Y, _show_list);
        draw_icon(MusicIconId::VolumeDown, 474, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::VolumeUp, 579, BOTTOM_CONTROL_Y);
    }

    void draw_all(epd_mode_t mode = epd_mode_t::epd_fast)
    {
        ensure_focus_visible();
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
        M5.Display.fillScreen(TFT_WHITE);

        std::string title;
        if (_show_list) title = "Radio Stations";
        else if (_current >= 0) title = STATIONS[_current].name;
        else title = "Network Radio";
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(title, design_x(530)),
                               design_x(TITLE_X), design_y(TITLE_Y), TFT_BLACK);

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
    ActivityManager::instance().push(new RadioActivity());
}
