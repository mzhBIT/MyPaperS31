#include "activities/SettingActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/EpdDiagnosticActivity.h"
#include "activities/RssReaderActivity.h"
#include "activities/TextInputActivity.h"
#include "deepseek/DeepSeekConfig.h"
#include "deepseek/DeepSeekClient.h"
#include "map/AmapConfig.h"
#include "storage/app_paths.h"
#include "ui/SystemFont.h"
#include "ui/VirtualKeyboard.h"
#include "wifi/wifi_setting.h"
#include "zhihu/ZhihuClient.h"
#include "zhihu/ZhihuConfig.h"

#include <M5Unified.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int HEADER_H = 76;
constexpr int PAD = 20;
constexpr int SETTINGS_ROW_Y = 106;
constexpr int SETTINGS_ROW_H = 126;
constexpr int SETTINGS_ROW_GAP = 14;
constexpr int SETTINGS_ITEM_COUNT = 6;

constexpr int SCAN_TOP = 94;
constexpr int SCAN_ROW_H = 92;
constexpr int SCAN_FOOTER_Y = 1102;
constexpr int SCAN_FOOTER_H = 82;

const char *TAG = "SETTINGS";

struct AccessPoint {
    std::string ssid;
    int rssi = -127;
    int channel = 0;
    bool secured = true;
};

void draw_header(const char *title)
{
    M5.Display.fillRect(0, 0, M5.Display.width(), HEADER_H, TFT_WHITE);
    system_font::draw_center(M5.Display, title, M5.Display.width() / 2,
                             std::max(0, (HEADER_H - system_font::line_height()) / 2),
                             TFT_BLACK);
    M5.Display.drawFastHLine(0, HEADER_H - 1, M5.Display.width(), TFT_BLACK);
}

bool header_back_hit(const TouchEvent &event)
{
    return event.type == TouchEventType::Tap && event.x < 120 && event.y < HEADER_H;
}

class WifiSettingsActivity final : public Activity {
public:
    ~WifiSettingsActivity() override
    {
        shutdown_wifi();
    }

    void on_create() override
    {
        scan_networks(true);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            go_back();
            return true;
        }

        if (_view == View::Scan) return scan_touch(event);
        if (_view == View::Keyboard) return keyboard_touch(event);
        if (_view == View::Result && event.type == TouchEventType::Tap) {
            if (_result_success) ActivityManager::instance().pop();
            else {
                _view = View::Keyboard;
                _keyboard_message = "Check the password and try again";
                draw_keyboard();
            }
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            go_back();
            return true;
        }
        if (_view == View::Scan) {
            if (_aps.empty()) {
                if (key == APP_KEY_OK) scan_networks(false);
                return true;
            }
            if (key == APP_KEY_UP) move_scan_focus(-1);
            else if (key == APP_KEY_DOWN) move_scan_focus(1);
            else if (key == APP_KEY_OK) select_access_point(_focus);
            else if (key == APP_KEY_EXTRA) scan_networks(false);
            return true;
        }
        if (_view == View::Keyboard) {
            if (key == APP_KEY_OK) connect_selected();
            else if (key == APP_KEY_EXTRA && _keyboard.erase_last()) refresh_keyboard_input();
            return true;
        }
        if (_view == View::Result && key == APP_KEY_OK) {
            if (_result_success) ActivityManager::instance().pop();
            else {
                _view = View::Keyboard;
                draw_keyboard();
            }
            return true;
        }
        return false;
    }

private:
    enum class View { Scan, Keyboard, Result };

    View _view = View::Scan;
    std::vector<AccessPoint> _aps;
    int _focus = 0;
    int _scroll = 0;
    int _selected = -1;
    bool _wifi_active = false;
    bool _result_success = false;
    ui::VirtualKeyboard _keyboard{WIFI_SETTING_PASSWORD_MAX_LEN};
    std::string _scan_message;
    std::string _keyboard_message;
    std::string _result_message;
    std::string _saved_ssid;

    void go_back()
    {
        if (_view == View::Keyboard || _view == View::Result) {
            _view = View::Scan;
            draw_scan();
        } else {
            ActivityManager::instance().pop();
        }
    }

    void load_saved_ssid()
    {
        char ssid[WIFI_SETTING_SSID_MAX_LEN + 1] = {};
        char password[WIFI_SETTING_PASSWORD_MAX_LEN + 1] = {};
        _saved_ssid.clear();
        if (wifi_setting_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
            _saved_ssid = ssid;
        }
    }

    void shutdown_wifi()
    {
        if (!_wifi_active) return;
        esp_err_t ret = wifi_setting_deinit(true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi shutdown after settings operation failed: %s",
                     esp_err_to_name(ret));
        }
        _wifi_active = false;
    }

    void draw_waiting(const char *message, bool quality)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("WiFi Settings");
        system_font::draw_center(M5.Display, message, M5.Display.width() / 2,
                                 M5.Display.height() / 2 - system_font::line_height(),
                                 TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
        M5.Display.waitDisplay();
    }

    void scan_networks(bool first_draw)
    {
        _view = View::Scan;
        _scan_message.clear();
        draw_waiting("Scanning nearby WiFi...", first_draw);

        wifi_setting_ap_t records[24] = {};
        size_t count = 0;
        _wifi_active = true;
        esp_err_t ret = wifi_setting_scan(records, 24, &count);
        shutdown_wifi();

        _aps.clear();
        if (ret == ESP_OK) {
            for (size_t i = 0; i < count; ++i) {
                auto existing = std::find_if(_aps.begin(), _aps.end(), [&](const AccessPoint &ap) {
                    return ap.ssid == records[i].ssid;
                });
                AccessPoint candidate{records[i].ssid, records[i].rssi,
                                      records[i].channel, records[i].secured};
                if (existing == _aps.end()) _aps.push_back(candidate);
                else if (candidate.rssi > existing->rssi) *existing = candidate;
            }
            std::sort(_aps.begin(), _aps.end(), [](const AccessPoint &a, const AccessPoint &b) {
                return a.rssi > b.rssi;
            });
            if (_aps.empty()) _scan_message = "No visible WiFi networks";
        } else {
            _scan_message = std::string("Scan failed: ") + esp_err_to_name(ret);
        }
        _focus = 0;
        _scroll = 0;
        load_saved_ssid();
        draw_scan();
    }

    int visible_scan_rows() const
    {
        return std::max(1, (SCAN_FOOTER_Y - SCAN_TOP) / SCAN_ROW_H);
    }

    void draw_scan()
    {
        int width = M5.Display.width();
        int rows = visible_scan_rows();
        int max_scroll = std::max(0, static_cast<int>(_aps.size()) - rows);
        _scroll = std::max(0, std::min(_scroll, max_scroll));

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("WiFi Settings");

        if (_aps.empty()) {
            system_font::draw_center(M5.Display,
                                     _scan_message.empty() ? "No WiFi networks" : _scan_message,
                                     width / 2, 260, TFT_BLACK);
        }

        for (int row = 0; row < rows; ++row) {
            int index = _scroll + row;
            if (index >= static_cast<int>(_aps.size())) break;
            int y = SCAN_TOP + row * SCAN_ROW_H;
            bool selected = index == _focus;
            uint16_t background = selected ? TFT_BLACK : TFT_WHITE;
            uint16_t foreground = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRoundRect(PAD, y, width - PAD * 2, SCAN_ROW_H - 7,
                                     7, background);
            M5.Display.drawRoundRect(PAD, y, width - PAD * 2, SCAN_ROW_H - 7,
                                     7, TFT_BLACK);
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(_aps[index].ssid, width - 250),
                                   PAD + 14, y + 5, foreground);
            char details[72] = {};
            snprintf(details, sizeof(details), "%d dBm  CH %d  %s%s",
                     _aps[index].rssi, _aps[index].channel,
                     _aps[index].secured ? "Locked" : "Open",
                     _aps[index].ssid == _saved_ssid ? "  Saved" : "");
            system_font::draw_left(M5.Display, details, PAD + 14, y + 45, foreground);
        }

        M5.Display.fillRoundRect(PAD, SCAN_FOOTER_Y,
                                 width - PAD * 2, SCAN_FOOTER_H, 8, TFT_WHITE);
        M5.Display.drawRoundRect(PAD, SCAN_FOOTER_Y,
                                 width - PAD * 2, SCAN_FOOTER_H, 8, TFT_BLACK);
        system_font::draw_center(M5.Display, "Rescan", width / 2,
                                 SCAN_FOOTER_Y +
                                     std::max(0, (SCAN_FOOTER_H - system_font::line_height()) / 2),
                                 TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void move_scan_focus(int delta)
    {
        if (_aps.empty()) return;
        int rows = visible_scan_rows();
        _focus = std::max(0, std::min(static_cast<int>(_aps.size()) - 1, _focus + delta));
        if (_focus < _scroll) _scroll = _focus;
        if (_focus >= _scroll + rows) _scroll = _focus - rows + 1;
        draw_scan();
    }

    bool scan_touch(const TouchEvent &event)
    {
        if (event.type == TouchEventType::SwipeUp) {
            move_scan_focus(visible_scan_rows());
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            move_scan_focus(-visible_scan_rows());
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y >= SCAN_FOOTER_Y) {
            scan_networks(false);
            return true;
        }
        if (event.y < SCAN_TOP) return false;
        int index = _scroll + (event.y - SCAN_TOP) / SCAN_ROW_H;
        if (index >= 0 && index < static_cast<int>(_aps.size())) {
            select_access_point(index);
            return true;
        }
        return false;
    }

    void select_access_point(int index)
    {
        if (index < 0 || index >= static_cast<int>(_aps.size())) return;
        _selected = index;
        _focus = index;
        _keyboard.reset();
        _keyboard.set_secret(true);
        _keyboard.set_placeholder("Password");
        _keyboard.set_submit_label("Connect & Save");
        _keyboard_message = _aps[index].secured
                                ? "Enter the network password"
                                : "Open network: no password required";
        _view = View::Keyboard;
        draw_keyboard();
    }

    void draw_keyboard()
    {
        if (_selected < 0 || _selected >= static_cast<int>(_aps.size())) return;
        int width = M5.Display.width();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("WiFi Password");
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(_aps[_selected].ssid, width - PAD * 2),
                                 width / 2, 104, TFT_BLACK);
        _keyboard.draw(M5.Display);
        system_font::draw_center(M5.Display, _keyboard_message, width / 2, 982, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void refresh_keyboard_input()
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        _keyboard.draw_input(M5.Display);
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool keyboard_touch(const TouchEvent &event)
    {
        if (event.type != TouchEventType::Tap) return false;
        switch (_keyboard.handle_tap(event.x, event.y, M5.Display.width())) {
            case ui::VirtualKeyboardAction::InputChanged:
                refresh_keyboard_input();
                return true;
            case ui::VirtualKeyboardAction::LayoutChanged:
                draw_keyboard();
                return true;
            case ui::VirtualKeyboardAction::LanguageChanged:
                return true;
            case ui::VirtualKeyboardAction::Submit:
                connect_selected();
                return true;
            case ui::VirtualKeyboardAction::LimitReached:
                _keyboard_message = "Password is limited to 63 characters";
                draw_keyboard();
                return true;
            case ui::VirtualKeyboardAction::None:
                break;
        }
        return false;
    }

    void connect_selected()
    {
        if (_selected < 0 || _selected >= static_cast<int>(_aps.size())) return;
        const AccessPoint &ap = _aps[_selected];
        if (ap.secured && _keyboard.empty()) {
            _keyboard_message = "Password is required for this network";
            draw_keyboard();
            return;
        }

        draw_waiting("Verifying WiFi connection...", false);
        _wifi_active = true;
        esp_err_t connect_ret = wifi_setting_connect(ap.ssid.c_str(), _keyboard.text().c_str(),
                                                     CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
        esp_err_t save_ret = ESP_OK;
        if (connect_ret == ESP_OK) {
            save_ret = wifi_setting_save_credentials(ap.ssid.c_str(),
                                                      _keyboard.text().c_str());
        }
        shutdown_wifi();

        _result_success = connect_ret == ESP_OK && save_ret == ESP_OK;
        if (_result_success) {
            _result_message = "Connected and saved as default";
            load_saved_ssid();
        } else if (connect_ret != ESP_OK) {
            _result_message = std::string("Connection failed: ") + esp_err_to_name(connect_ret);
        } else {
            _result_message = std::string("NVS save failed: ") + esp_err_to_name(save_ret);
        }
        _view = View::Result;
        draw_result();
    }

    void draw_result()
    {
        int width = M5.Display.width();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("WiFi Settings");
        system_font::draw_center(M5.Display, _result_success ? "WiFi Ready" : "WiFi Error",
                                 width / 2, 300, TFT_BLACK);
        if (_selected >= 0 && _selected < static_cast<int>(_aps.size())) {
            system_font::draw_center(M5.Display,
                                     system_font::ellipsize(_aps[_selected].ssid, width - PAD * 2),
                                     width / 2, 390, TFT_BLACK);
        }
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(_result_message, width - PAD * 2),
                                 width / 2, 480, TFT_BLACK);

        const char *action = _result_success ? "Done" : "Retry Password";
        M5.Display.fillRoundRect(80, 650, width - 160, 90, 9, TFT_BLACK);
        system_font::draw_center(M5.Display, action, width / 2,
                                 650 + std::max(0, (90 - system_font::line_height()) / 2),
                                 TFT_WHITE);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class DeepSeekSettingsActivity final : public Activity {
public:
    void on_create() override { draw(true); }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        draw(false);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y >= 130 && event.y < 270) {
            _focus = 0;
            edit_api_key();
            return true;
        }
        if (event.y >= 290 && event.y < 430) {
            _focus = 1;
            toggle_model();
            return true;
        }
        if (event.y >= 450 && event.y < 590) {
            _focus = 2;
            test_api_key();
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
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            _focus = (_focus + (key == APP_KEY_DOWN ? 1 : 2)) % 3;
            draw(false);
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_focus == 0) edit_api_key();
            else if (_focus == 1) toggle_model();
            else test_api_key();
            return true;
        }
        return false;
    }

private:
    bool _seen_show = false;
    int _focus = 0;
    std::string _message;

    void edit_api_key()
    {
        TextInputOptions options;
        options.title = "DeepSeek API Key";
        options.submit_label = "Save";
        options.max_bytes = 255;
        options.start_in_chinese = false;
        options.secret = true;
        launch_text_input(options, [](const std::string &value) {
            if (!deepseek_config_save_api_key(value)) {
                ESP_LOGE(TAG, "Failed to save DeepSeek API key");
            }
        });
    }

    void toggle_model()
    {
        DeepSeekConfig config;
        deepseek_config_load(&config);
        deepseek_config_save_default_model(deepseek_model_toggle(config.default_model));
        draw(false);
    }

    void test_api_key()
    {
        if (!ensure_wifi_configured_or_show_guide()) return;
        DeepSeekConfig config;
        deepseek_config_load(&config);
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("DeepSeek Settings");
        system_font::draw_center(M5.Display, "Validating API key...",
                                 M5.Display.width() / 2, 420, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
        M5.Display.waitDisplay();
        DeepSeekClientResult result = DeepSeekClient::validate(config);
        _message = result.success ? result.content : result.error;
        draw(false);
    }

    void draw_row(int y, const char *title, const std::string &detail, bool selected)
    {
        int width = M5.Display.width();
        uint16_t bg = selected ? TFT_BLACK : TFT_WHITE;
        uint16_t fg = selected ? TFT_WHITE : TFT_BLACK;
        M5.Display.fillRoundRect(PAD, y, width - PAD * 2, 140, 8, bg);
        M5.Display.drawRoundRect(PAD, y, width - PAD * 2, 140, 8, TFT_BLACK);
        system_font::draw_left(M5.Display, title, PAD + 18, y + 14, fg);
        system_font::draw_left(M5.Display, detail, PAD + 18, y + 74, fg);
        system_font::draw_right(M5.Display, ">", width - PAD - 20, y + 48, fg);
    }

    void draw(bool quality)
    {
        DeepSeekConfig config;
        bool has_key = deepseek_config_load(&config);
        std::string key_detail = has_key ? "Configured (tap to replace)"
                                         : "Not configured";
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("DeepSeek Settings");
        draw_row(130, "API Key", key_detail, _focus == 0);
        draw_row(290, "Default Model", deepseek_model_label(config.default_model), _focus == 1);
        draw_row(450, "Test Connection", "Validate Flash and Pro access", _focus == 2);
        system_font::draw_center(M5.Display,
                                 _message.empty() ? "The API key is stored in device NVS" :
                                                    system_font::ellipsize(_message, M5.Display.width() - 40),
                                 M5.Display.width() / 2, 630, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class ZhihuSettingsActivity final : public Activity {
public:
    void on_create() override { draw(true); }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        draw(false);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y >= 170 && event.y < 310) {
            _focus = 0;
            edit_access_secret();
            return true;
        }
        if (event.y >= 330 && event.y < 470) {
            _focus = 1;
            test_connection();
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
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            _focus = 1 - _focus;
            draw(false);
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_focus == 0) edit_access_secret();
            else test_connection();
            return true;
        }
        return false;
    }

private:
    bool _seen_show = false;
    int _focus = 0;
    std::string _message;

    void edit_access_secret()
    {
        TextInputOptions options;
        options.title = "Zhihu Access Secret";
        options.submit_label = "Save";
        options.max_bytes = 255;
        options.start_in_chinese = false;
        options.secret = true;
        launch_text_input(options, [](const std::string &value) {
            if (!zhihu_config_save_access_secret(value)) {
                ESP_LOGE(TAG, "Failed to save Zhihu Access Secret");
            }
        });
    }

    void test_connection()
    {
        if (!ensure_wifi_configured_or_show_guide()) return;
        ZhihuConfig config;
        zhihu_config_load(&config);
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("ZhiHu Settings");
        system_font::draw_center(M5.Display, "Loading Zhihu hot list...",
                                 M5.Display.width() / 2, 420, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
        M5.Display.waitDisplay();
        ZhihuHotListResult result = ZhihuClient::fetch_hot_list(config, 1, true);
        _message = result.success ? "Connection successful" : result.error;
        draw(false);
    }

    void draw_row(int y, const char *title, const std::string &detail, bool selected)
    {
        const int width = M5.Display.width();
        const uint16_t background = selected ? TFT_BLACK : TFT_WHITE;
        const uint16_t foreground = selected ? TFT_WHITE : TFT_BLACK;
        M5.Display.fillRoundRect(PAD, y, width - PAD * 2, 140, 8, background);
        M5.Display.drawRoundRect(PAD, y, width - PAD * 2, 140, 8, TFT_BLACK);
        system_font::draw_left(M5.Display, title, PAD + 18, y + 14, foreground);
        system_font::draw_left(M5.Display, detail, PAD + 18, y + 74, foreground);
        system_font::draw_right(M5.Display, ">", width - PAD - 20, y + 48, foreground);
    }

    void draw(bool quality)
    {
        ZhihuConfig config;
        const bool configured = zhihu_config_load(&config);
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("ZhiHu Settings");
        draw_row(170, "Access Secret",
                 configured ? "Configured (tap to replace)" : "Not configured",
                 _focus == 0);
        draw_row(330, "Test Connection", "Request one hot-list item", _focus == 1);
        system_font::draw_center(M5.Display,
                                 _message.empty() ? "The secret is stored in device NVS" :
                                                    system_font::ellipsize(_message,
                                                                           M5.Display.width() - 40),
                                 M5.Display.width() / 2, 540, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class MapSettingsActivity final : public Activity {
public:
    void on_create() override { draw(true); }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        draw(false);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        if (event.y >= 180 && event.y < 320) {
            _focus = 0;
            edit_api_key();
            return true;
        }
        if (event.y >= 340 && event.y < 480) {
            _focus = 1;
            clear_cache();
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
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            _focus = 1 - _focus;
            draw(false);
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_focus == 0) edit_api_key();
            else clear_cache();
            return true;
        }
        return false;
    }

private:
    bool _seen_show = false;
    int _focus = 0;
    std::string _message;

    void edit_api_key()
    {
        TextInputOptions options;
        options.title = "AMap Web Service Key";
        options.submit_label = "Save";
        options.max_bytes = 255;
        options.start_in_chinese = false;
        options.secret = true;
        launch_text_input(options, [](const std::string &value) {
            if (!amap_config_save_api_key(value)) {
                ESP_LOGE(TAG, "Failed to save AMap API key");
            } else {
                ESP_LOGI(TAG, "AMap API key stored length=%u",
                         static_cast<unsigned>(value.size()));
            }
        });
    }

    void clear_cache()
    {
        const int removed = std::remove(app_paths::map_image_file());
        _message = removed == 0 ? "Map cache cleared" : "Map cache is already empty";
        draw(false);
    }

    void draw_row(int y, const char *title, const std::string &detail, bool selected)
    {
        const int width = M5.Display.width();
        const uint16_t background = selected ? TFT_BLACK : TFT_WHITE;
        const uint16_t foreground = selected ? TFT_WHITE : TFT_BLACK;
        M5.Display.fillRoundRect(PAD, y, width - PAD * 2, 140, 8, background);
        M5.Display.drawRoundRect(PAD, y, width - PAD * 2, 140, 8, TFT_BLACK);
        system_font::draw_left(M5.Display, title, PAD + 18, y + 14, foreground);
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(detail, width - PAD * 2 - 80),
                               PAD + 18, y + 74, foreground);
        system_font::draw_right(M5.Display, ">", width - PAD - 20, y + 48, foreground);
    }

    void draw(bool quality)
    {
        AmapConfig config;
        const bool configured = amap_config_load(&config);
        FILE *cache = std::fopen(app_paths::map_image_file(), "rb");
        const bool cached = cache != nullptr;
        if (cache) std::fclose(cache);

        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("Map Settings");
        draw_row(180, "AMap API Key",
                 configured ? "Configured (tap to replace)" : "Not configured",
                 _focus == 0);
        draw_row(340, "Clear Map Cache",
                 cached ? "Cached map is available" : "No cached map",
                 _focus == 1);
        system_font::draw_center(M5.Display,
                                 _message.empty() ? "The API key is stored in device NVS"
                                                  : _message,
                                 M5.Display.width() / 2, 550, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

static std::string format_cache_bytes(uint64_t bytes)
{
    char text[32] = {};
    if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(text, sizeof(text), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        std::snprintf(text, sizeof(text), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return text;
}

class RssCacheSettingsActivity final : public Activity {
public:
    void on_create() override
    {
        refresh_info();
        draw(true);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            if (_confirm) {
                _confirm = false;
                draw(false);
            } else {
                ActivityManager::instance().pop();
            }
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;

        if (!_confirm) {
            if (event.y >= 430 && event.y < 530) begin_confirm();
            return true;
        }

        const int width = M5.Display.width();
        if (event.y >= 790 && event.y < 880) {
            if (event.x < width / 2) cancel_confirm();
            else clear_cache();
        }
        return true;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            if (_confirm) cancel_confirm();
            else ActivityManager::instance().pop();
            return true;
        }
        if (_confirm && (key == APP_KEY_UP || key == APP_KEY_DOWN)) {
            _confirm_choice = 1 - _confirm_choice;
            draw(false);
            return true;
        }
        if (key == APP_KEY_OK) {
            if (!_confirm) begin_confirm();
            else if (_confirm_choice == 0) cancel_confirm();
            else clear_cache();
            return true;
        }
        return false;
    }

private:
    RssCacheInfo _info;
    bool _confirm = false;
    int _confirm_choice = 0;
    std::string _message;

    void refresh_info()
    {
        _info = rss_cache_info();
    }

    void begin_confirm()
    {
        _confirm = true;
        _confirm_choice = 0;
        draw(false);
    }

    void cancel_confirm()
    {
        _confirm = false;
        _confirm_choice = 0;
        draw(false);
    }

    void clear_cache()
    {
        RssCacheInfo removed;
        const bool ok = clear_rss_cache(&removed);
        refresh_info();
        _confirm = false;
        _confirm_choice = 0;
        char message[80] = {};
        std::snprintf(message, sizeof(message),
                      ok ? "Cleared %u files, %s" : "Partially cleared %u files, %s",
                      static_cast<unsigned>(removed.files),
                      format_cache_bytes(removed.bytes).c_str());
        _message = message;
        draw(false);
    }

    void draw(bool quality)
    {
        const int width = M5.Display.width();
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("RSS Cache");

        char summary[80] = {};
        std::snprintf(summary, sizeof(summary), "%u image files, %s",
                      static_cast<unsigned>(_info.files),
                      format_cache_bytes(_info.bytes).c_str());
        system_font::draw_center(M5.Display, summary, width / 2, 180, TFT_BLACK);
        system_font::draw_center(M5.Display, "Removes downloaded RSS images and temporary article data",
                                 width / 2, 260, TFT_BLACK);
        system_font::draw_center(M5.Display, "Subscriptions and saved Bing wallpapers are kept",
                                 width / 2, 320, TFT_BLACK);

        M5.Display.fillRoundRect(70, 430, width - 140, 100, 8, TFT_BLACK);
        system_font::draw_center(M5.Display, "Clear RSS Cache", width / 2,
                                 430 + std::max(0, (100 - system_font::line_height()) / 2),
                                 TFT_WHITE);
        if (!_message.empty()) {
            system_font::draw_center(M5.Display,
                                     system_font::ellipsize(_message, width - 50),
                                     width / 2, 590, TFT_BLACK);
        }

        if (_confirm) {
            M5.Display.fillRoundRect(35, 665, width - 70, 250, 10, TFT_WHITE);
            M5.Display.drawRoundRect(35, 665, width - 70, 250, 10, TFT_BLACK);
            system_font::draw_center(M5.Display, "Clear all RSS temporary cache?",
                                     width / 2, 700, TFT_BLACK);
            const int button_y = 790;
            const int button_w = (width - 105) / 2;
            const int left_x = 45;
            const int right_x = left_x + button_w + 15;
            M5.Display.fillRoundRect(left_x, button_y, button_w, 90, 8,
                                     _confirm_choice == 0 ? TFT_BLACK : TFT_WHITE);
            M5.Display.drawRoundRect(left_x, button_y, button_w, 90, 8, TFT_BLACK);
            M5.Display.fillRoundRect(right_x, button_y, button_w, 90, 8,
                                     _confirm_choice == 1 ? TFT_BLACK : TFT_WHITE);
            M5.Display.drawRoundRect(right_x, button_y, button_w, 90, 8, TFT_BLACK);
            system_font::draw_center(M5.Display, "Cancel", left_x + button_w / 2,
                                     button_y + 20,
                                     _confirm_choice == 0 ? TFT_WHITE : TFT_BLACK);
            system_font::draw_center(M5.Display, "Clear", right_x + button_w / 2,
                                     button_y + 20,
                                     _confirm_choice == 1 ? TFT_WHITE : TFT_BLACK);
        }

        M5.Display.display();
        M5.Display.endWrite();
    }
};

class SettingActivity final : public Activity {
public:
    void on_create() override
    {
        draw(true);
    }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        draw(false);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::Tap) {
            for (int i = 0; i < SETTINGS_ITEM_COUNT; ++i) {
                int top = SETTINGS_ROW_Y + i * (SETTINGS_ROW_H + SETTINGS_ROW_GAP);
                if (event.y >= top && event.y < top + SETTINGS_ROW_H) {
                    _focus = i;
                    open_focused();
                    return true;
                }
            }
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP) {
            _focus = (_focus + SETTINGS_ITEM_COUNT - 1) % SETTINGS_ITEM_COUNT;
            draw(false);
            return true;
        }
        if (key == APP_KEY_DOWN) {
            _focus = (_focus + 1) % SETTINGS_ITEM_COUNT;
            draw(false);
            return true;
        }
        if (key == APP_KEY_OK) {
            open_focused();
            return true;
        }
        return false;
    }

private:
    bool _seen_show = false;
    int _focus = 0;

    void open_focused()
    {
        if (_focus == 0) ActivityManager::instance().push(new WifiSettingsActivity());
        else if (_focus == 1) ActivityManager::instance().push(new DeepSeekSettingsActivity());
        else if (_focus == 2) ActivityManager::instance().push(new ZhihuSettingsActivity());
        else if (_focus == 3) ActivityManager::instance().push(new MapSettingsActivity());
        else if (_focus == 4) launch_epd_diagnostics();
        else ActivityManager::instance().push(new RssCacheSettingsActivity());
    }

    void draw_item(int index, const char *title, const std::string &detail)
    {
        const int width = M5.Display.width();
        const int top = SETTINGS_ROW_Y + index * (SETTINGS_ROW_H + SETTINGS_ROW_GAP);
        const bool selected = index == _focus;
        const uint16_t background = selected ? TFT_BLACK : TFT_WHITE;
        const uint16_t foreground = selected ? TFT_WHITE : TFT_BLACK;
        M5.Display.fillRoundRect(PAD, top, width - PAD * 2,
                                 SETTINGS_ROW_H, 8, background);
        M5.Display.drawRoundRect(PAD, top, width - PAD * 2,
                                 SETTINGS_ROW_H, 8, TFT_BLACK);
        system_font::draw_left(M5.Display, title, PAD + 18,
                               top + 10, foreground);
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(detail, width - PAD * 2 - 80),
                               PAD + 18, top + 62, foreground);
        system_font::draw_right(M5.Display, ">", width - PAD - 20,
                                top + 36, foreground);
    }

    void draw(bool quality)
    {
        char ssid[WIFI_SETTING_SSID_MAX_LEN + 1] = {};
        char password[WIFI_SETTING_PASSWORD_MAX_LEN + 1] = {};
        bool configured = wifi_setting_load_credentials(ssid, sizeof(ssid),
                                                        password, sizeof(password));
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("Settings");
        std::string detail = configured ? std::string("Default: ") + ssid
                                        : "Not configured";
        draw_item(0, "WiFi", detail);
        DeepSeekConfig deepseek;
        bool has_api_key = deepseek_config_load(&deepseek);
        draw_item(1, "DeepSeek",
                  has_api_key ? std::string("Configured, ") + deepseek_model_label(deepseek.default_model)
                              : "API key not configured");
        ZhihuConfig zhihu;
        draw_item(2, "ZhiHu",
                  zhihu_config_load(&zhihu) ? "Access Secret configured"
                                            : "Access Secret not configured");
        AmapConfig amap;
        draw_item(3, "Map",
                  amap_config_load(&amap) ? "AMap API key configured"
                                          : "AMap API key not configured");
        draw_item(4, "EPD Diagnostics",
                  "Direction, grayscale, transitions and VCOM");
        RssCacheInfo rss_cache = rss_cache_info();
        char rss_detail[64] = {};
        std::snprintf(rss_detail, sizeof(rss_detail), "%u files, %s",
                      static_cast<unsigned>(rss_cache.files),
                      format_cache_bytes(rss_cache.bytes).c_str());
        draw_item(5, "Clear RSS Cache", rss_detail);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class WifiSetupGuideActivity final : public Activity {
public:
    void on_create() override
    {
        _exit_at_us = esp_timer_get_time() + 2500000;
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("WiFi Required");
        system_font::draw_center(M5.Display, "WiFi is not configured",
                                 M5.Display.width() / 2, 350, TFT_BLACK);
        system_font::draw_center(M5.Display, "Open Setting > WiFi first",
                                 M5.Display.width() / 2, 440, TFT_BLACK);
        system_font::draw_center(M5.Display, "Returning...",
                                 M5.Display.width() / 2, 560, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void loop() override
    {
        if (esp_timer_get_time() >= _exit_at_us) close();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Tap || event.type == TouchEventType::Back) {
            close();
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK || key == APP_KEY_OK) {
            close();
            return true;
        }
        return false;
    }

private:
    int64_t _exit_at_us = 0;
    bool _closing = false;

    void close()
    {
        if (_closing) return;
        _closing = true;
        ActivityManager::instance().pop();
    }
};

}  // namespace

void launch_settings()
{
    ActivityManager::instance().push(new SettingActivity());
}

bool ensure_wifi_configured_or_show_guide()
{
    char ssid[WIFI_SETTING_SSID_MAX_LEN + 1] = {};
    char password[WIFI_SETTING_PASSWORD_MAX_LEN + 1] = {};
    if (wifi_setting_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        return true;
    }
    ActivityManager::instance().push(new WifiSetupGuideActivity());
    return false;
}
