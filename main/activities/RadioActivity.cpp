#include "activities/RadioActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "audio/audio_radio.h"
#include "ui/SystemFont.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

static const char *TAG = "RADIO";
static const char *WIFI_CONFIG_PATH = "/sdcard/wifi.txt";
static const int TOP_H = 54;
static const int ROW_H = 58;
static const int PAD = 12;

struct RadioStation {
    const char *name;
    const char *url;
};

static const RadioStation STATIONS[] = {
    {"CNR中国之声", "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
    {"CNR经济之声", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8"},
    {"CNR文艺之声", "http://ngcdn001.cnr.cn/live/wyzs/index.m3u8"},
    {"CNR阅读之声", "http://ngcdn014.cnr.cn/live/ylgb/index.m3u8"},
    {"CNR台海之声", "http://ngcdn002.cnr.cn/live/zhzs/index.m3u8"},
    {"CNR乡村之声", "http://ngcdn002.cnr.cn/live/xczs/index.m3u8"},
    {"CNR老年之声", "http://ngcdn002.cnr.cn/live/lnzs/index.m3u8"},
    {"CNR民族之声", "http://ngcdn002.cnr.cn/live/mzzs/index.m3u8"},
    {"CNR香港之声", "http://ngcdn002.cnr.cn/live/xgzs/index.m3u8"},
    {"CNR大湾区之声", "http://ngcdn002.cnr.cn/live/hxzs/index.m3u8"},
    {"CNR神州之声", "http://ngcdn002.cnr.cn/live/szzs/index.m3u8"},
    {"CNR中国交通广播", "http://ngcdn002.cnr.cn/live/gsgljtgb/index.m3u8"},
    {"CNR音乐之声", "http://ngcdn002.cnr.cn/live/yyzs/index.m3u8"},
    {"CNR经典音乐广播", "http://ngcdn002.cnr.cn/live/dszs/index.m3u8"},
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

class RadioActivity : public Activity {
public:
    ~RadioActivity() override
    {
        audio_radio_deinit();
        wifi_setting_deinit(false);
    }

    void on_create() override
    {
        draw_status("Radio", "Connecting WiFi...");
        _wifi_ok = connect_wifi();
        if (!_wifi_ok) {
            draw_status("Radio WiFi", "Create /sdcard/wifi.txt");
            return;
        }
        draw_list();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y < TOP_H) {
            ActivityManager::instance().pop();
            return true;
        }
        int idx = hit_station(event.y);
        if (idx < 0) return false;
        _focus = idx;
        play_focused();
        return true;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP && _focus > 0) {
            _focus--;
            ensure_focus_visible();
            draw_list();
            return true;
        }
        if (key == APP_KEY_DOWN && _focus + 1 < STATION_COUNT) {
            _focus++;
            ensure_focus_visible();
            draw_list();
            return true;
        }
        if (key == APP_KEY_OK) {
            play_focused();
            return true;
        }
        return false;
    }

private:
    bool _wifi_ok = false;
    bool _playing = false;
    int _focus = 0;
    int _scroll = 0;
    std::string _status;

    int visible_rows() const
    {
        return std::max(1, (int)(M5.Display.height() - TOP_H - PAD) / ROW_H);
    }

    void ensure_focus_visible()
    {
        int rows = visible_rows();
        if (_focus < _scroll) _scroll = _focus;
        if (_focus >= _scroll + rows) _scroll = _focus - rows + 1;
        _scroll = std::max(0, std::min(_scroll, std::max(0, STATION_COUNT - rows)));
    }

    int hit_station(int y) const
    {
        int row = (y - TOP_H) / ROW_H;
        int idx = _scroll + row;
        return idx >= 0 && idx < STATION_COUNT ? idx : -1;
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
        esp_err_t ret = wifi_setting_connect(ssid.c_str(), password.c_str(), CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(ret));
            return false;
        }
        return true;
    }

    void draw_status(const char *title, const char *message)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.fillRect(0, 0, M5.Display.width(), TOP_H, TFT_BLACK);
        system_font::draw_left(M5.Display, "<", PAD, 12, TFT_WHITE);
        system_font::draw_center(M5.Display, title, M5.Display.width() / 2, 12, TFT_WHITE);
        system_font::draw_center(M5.Display, message, M5.Display.width() / 2, M5.Display.height() / 2 - 18);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_list()
    {
        ensure_focus_visible();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.fillRect(0, 0, M5.Display.width(), TOP_H, TFT_BLACK);
        system_font::draw_left(M5.Display, "<", PAD, 12, TFT_WHITE);
        system_font::draw_center(M5.Display, "Network Radio", M5.Display.width() / 2, 12, TFT_WHITE);

        int rows = visible_rows();
        int width = M5.Display.width() - PAD * 2;
        for (int i = 0; i < rows; ++i) {
            int idx = _scroll + i;
            if (idx >= STATION_COUNT) break;
            int y = TOP_H + i * ROW_H;
            bool selected = idx == _focus;
            bool current = idx == _current;
            uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
            uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRect(PAD, y + 4, width, ROW_H - 8, bg);
            M5.Display.drawRect(PAD, y + 4, width, ROW_H - 8, TFT_BLACK);
            std::string label = STATIONS[idx].name;
            if (current && _playing) label = std::string("> ") + label;
            system_font::draw_left(M5.Display, system_font::ellipsize(label, width - 24), PAD + 12, y + 16, fg);
        }

        if (!_status.empty()) {
            M5.Display.fillRect(0, M5.Display.height() - 34, M5.Display.width(), 34, TFT_WHITE);
            system_font::draw_center(M5.Display, system_font::ellipsize(_status, M5.Display.width() - 20), M5.Display.width() / 2, M5.Display.height() - 30);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }

    void play_focused()
    {
        draw_status(STATIONS[_focus].name, "Starting...");
        esp_err_t ret = audio_radio_play(STATIONS[_focus].url);
        if (ret == ESP_OK) {
            _current = _focus;
            _playing = true;
            _status = std::string("Playing: ") + STATIONS[_focus].name;
        } else {
            _playing = false;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Play failed: %s", esp_err_to_name(ret));
            _status = buf;
        }
        draw_list();
    }

    int _current = -1;
};

void launch_radio()
{
    ActivityManager::instance().push(new RadioActivity());
}
