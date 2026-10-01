#include "activities/SettingActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/EpdDiagnosticActivity.h"
#include "activities/RssReaderActivity.h"
#include "activities/TextInputActivity.h"
#include "deepseek/DeepSeekConfig.h"
#include "deepseek/DeepSeekClient.h"
#include "frontlight/frontlight.h"
#include "map/AmapConfig.h"
#include "storage/app_paths.h"
#include "time/time_basis.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "ui/VirtualKeyboard.h"
#include "ui/home_preferences.h"
#include "wifi/wifi_setting.h"
#include "zhihu/ZhihuClient.h"
#include "zhihu/ZhihuConfig.h"

#include <M5Unified.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_netif_sntp.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr int HEADER_H = 76;
constexpr int PAD = 20;
constexpr int SETTINGS_ROW_Y = HEADER_H;
constexpr int SETTINGS_ROW_H = 110;
constexpr int SETTINGS_ROW_GAP = 0;
constexpr int SETTINGS_ITEM_COUNT = 8;
constexpr int SETTINGS_VISIBLE_ROWS = 8;

constexpr int SCAN_TOP = HEADER_H;
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
                _keyboard_message = ui_locale::text(ui_locale::TextId::CheckPasswordHint);
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
        esp_err_t ret = wifi_setting_release();
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
        draw_header(ui_locale::text(ui_locale::TextId::WifiSettings));
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
        draw_waiting(ui_locale::text(ui_locale::TextId::ScanningWifi), first_draw);

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
            if (_aps.empty()) _scan_message = ui_locale::text(ui_locale::TextId::NoVisibleWifi);
        } else {
            _scan_message = std::string(ui_locale::text(ui_locale::TextId::ScanFailed)) + esp_err_to_name(ret);
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
        draw_header(ui_locale::text(ui_locale::TextId::WifiSettings));

        if (_aps.empty()) {
            system_font::draw_center(M5.Display,
                                     _scan_message.empty() ? ui_locale::text(ui_locale::TextId::NoWifiNetworks) : _scan_message,
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
            const std::string saved = _aps[index].ssid == _saved_ssid
                                          ? std::string("  ") + ui_locale::text(ui_locale::TextId::Saved)
                                          : std::string();
            snprintf(details, sizeof(details), "%d dBm  CH %d  %s%s",
                     _aps[index].rssi, _aps[index].channel,
                     _aps[index].secured ? ui_locale::text(ui_locale::TextId::Locked)
                                         : ui_locale::text(ui_locale::TextId::OpenNetwork),
                     saved.c_str());
            system_font::draw_left(M5.Display, details, PAD + 14, y + 45, foreground);
        }

        M5.Display.fillRoundRect(PAD, SCAN_FOOTER_Y,
                                 width - PAD * 2, SCAN_FOOTER_H, 8, TFT_WHITE);
        M5.Display.drawRoundRect(PAD, SCAN_FOOTER_Y,
                                 width - PAD * 2, SCAN_FOOTER_H, 8, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Rescan), width / 2,
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
        _keyboard.set_placeholder(ui_locale::text(ui_locale::TextId::Password));
        _keyboard.set_submit_label(ui_locale::text(ui_locale::TextId::ConnectAndSave));
        _keyboard_message = _aps[index].secured
                                ? ui_locale::text(ui_locale::TextId::EnterNetworkPassword)
                                : ui_locale::text(ui_locale::TextId::OpenNetworkNoPassword);
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
        draw_header(ui_locale::text(ui_locale::TextId::WifiPassword));
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
            case ui::VirtualKeyboardAction::CursorMoved:
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
                _keyboard_message = ui_locale::text(ui_locale::TextId::PasswordLengthHint);
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
            _keyboard_message = ui_locale::text(ui_locale::TextId::PasswordRequired);
            draw_keyboard();
            return;
        }

        draw_waiting(ui_locale::text(ui_locale::TextId::VerifyingWifi), false);
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
            _result_message = ui_locale::text(ui_locale::TextId::ConnectedAndSaved);
            load_saved_ssid();
        } else if (connect_ret != ESP_OK) {
            _result_message = std::string(ui_locale::text(ui_locale::TextId::ConnectionFailed)) + esp_err_to_name(connect_ret);
        } else {
            _result_message = std::string(ui_locale::text(ui_locale::TextId::NvsSaveFailed)) + esp_err_to_name(save_ret);
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
        draw_header(ui_locale::text(ui_locale::TextId::WifiSettings));
        system_font::draw_center(M5.Display, _result_success ? ui_locale::text(ui_locale::TextId::WifiReady)
                                                       : ui_locale::text(ui_locale::TextId::WifiError),
                                 width / 2, 300, TFT_BLACK);
        if (_selected >= 0 && _selected < static_cast<int>(_aps.size())) {
            system_font::draw_center(M5.Display,
                                     system_font::ellipsize(_aps[_selected].ssid, width - PAD * 2),
                                     width / 2, 390, TFT_BLACK);
        }
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(_result_message, width - PAD * 2),
                                 width / 2, 480, TFT_BLACK);

        const char *action = _result_success ? ui_locale::text(ui_locale::TextId::Done)
                                             : ui_locale::text(ui_locale::TextId::RetryPassword);
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
        if (event.y >= HEADER_H && event.y < HEADER_H + 140) {
            _focus = 0;
            edit_api_key();
            return true;
        }
        if (event.y >= HEADER_H + 160 && event.y < HEADER_H + 300) {
            _focus = 1;
            toggle_model();
            return true;
        }
        if (event.y >= HEADER_H + 320 && event.y < HEADER_H + 460) {
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
        options.title = ui_locale::text(ui_locale::TextId::DeepSeekApiKey);
        options.submit_label = ui_locale::text(ui_locale::TextId::Save);
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
        draw_header(ui_locale::text(ui_locale::TextId::DeepSeekSettings));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ValidatingApiKey),
                                 M5.Display.width() / 2, 420, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
        M5.Display.waitDisplay();
        DeepSeekClientResult result = DeepSeekClient::validate(config);
        _message = ui_locale::localize_message(result.success ? result.content : result.error);
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
        std::string key_detail = has_key ? ui_locale::text(ui_locale::TextId::ConfiguredTapReplace)
                                         : ui_locale::text(ui_locale::TextId::NotConfigured);
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::DeepSeekSettings));
        draw_row(HEADER_H, ui_locale::text(ui_locale::TextId::DeepSeekApiKey), key_detail, _focus == 0);
        draw_row(HEADER_H + 160, ui_locale::text(ui_locale::TextId::DefaultModel), deepseek_model_label(config.default_model), _focus == 1);
        draw_row(HEADER_H + 320, ui_locale::text(ui_locale::TextId::TestConnection), ui_locale::text(ui_locale::TextId::ValidateModelsHint), _focus == 2);
        system_font::draw_center(M5.Display,
                                 _message.empty() ? ui_locale::text(ui_locale::TextId::ApiKeyNvsHint) :
                                                    system_font::ellipsize(_message, M5.Display.width() - 40),
                                 M5.Display.width() / 2, HEADER_H + 500, TFT_BLACK);
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
        if (event.y >= HEADER_H && event.y < HEADER_H + 140) {
            _focus = 0;
            edit_access_secret();
            return true;
        }
        if (event.y >= HEADER_H + 160 && event.y < HEADER_H + 300) {
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
        options.title = ui_locale::text(ui_locale::TextId::ZhihuAccessSecret);
        options.submit_label = ui_locale::text(ui_locale::TextId::Save);
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
        draw_header(ui_locale::text(ui_locale::TextId::ZhihuSettings));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::LoadingZhihuHotList),
                                 M5.Display.width() / 2, 420, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
        M5.Display.waitDisplay();
        ZhihuHotListResult result = ZhihuClient::fetch_hot_list(config, 1, true);
        _message = result.success ? ui_locale::text(ui_locale::TextId::ConnectionSuccessful) : ui_locale::localize_message(result.error);
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
        draw_header(ui_locale::text(ui_locale::TextId::ZhihuSettings));
        draw_row(HEADER_H, ui_locale::text(ui_locale::TextId::AccessSecret),
                 configured ? ui_locale::text(ui_locale::TextId::ConfiguredTapReplace)
                            : ui_locale::text(ui_locale::TextId::NotConfigured),
                 _focus == 0);
        draw_row(HEADER_H + 160, ui_locale::text(ui_locale::TextId::TestConnection),
                 ui_locale::text(ui_locale::TextId::RequestHotListHint), _focus == 1);
        system_font::draw_center(M5.Display,
                                 _message.empty() ? ui_locale::text(ui_locale::TextId::SecretNvsHint) :
                                                    system_font::ellipsize(_message,
                                                                           M5.Display.width() - 40),
                                 M5.Display.width() / 2, HEADER_H + 370, TFT_BLACK);
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
        if (event.y >= HEADER_H && event.y < HEADER_H + 140) {
            _focus = 0;
            edit_api_key();
            return true;
        }
        if (event.y >= HEADER_H + 160 && event.y < HEADER_H + 300) {
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
        options.title = ui_locale::text(ui_locale::TextId::AmapWebServiceKey);
        options.submit_label = ui_locale::text(ui_locale::TextId::Save);
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
        _message = removed == 0 ? ui_locale::text(ui_locale::TextId::MapCacheCleared)
                                : ui_locale::text(ui_locale::TextId::MapCacheEmpty);
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
        draw_header(ui_locale::text(ui_locale::TextId::MapSettings));
        draw_row(HEADER_H, ui_locale::text(ui_locale::TextId::AmapApiKey),
                 configured ? ui_locale::text(ui_locale::TextId::ConfiguredTapReplace)
                            : ui_locale::text(ui_locale::TextId::NotConfigured),
                 _focus == 0);
        draw_row(HEADER_H + 160, ui_locale::text(ui_locale::TextId::ClearMapCache),
                 cached ? ui_locale::text(ui_locale::TextId::CachedMapAvailable)
                        : ui_locale::text(ui_locale::TextId::NoCachedMap),
                 _focus == 1);
        system_font::draw_center(M5.Display,
                                 _message.empty() ? ui_locale::text(ui_locale::TextId::ApiKeyNvsHint)
                                                  : _message,
                                 M5.Display.width() / 2, HEADER_H + 370, TFT_BLACK);
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
            if (event.y >= 340 && event.y < 440) begin_confirm();
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
        std::snprintf(message, sizeof(message), "%s %u %s, %s",
                      ui_locale::text(ok ? ui_locale::TextId::ClearedFiles
                                         : ui_locale::TextId::PartiallyClearedFiles),
                      static_cast<unsigned>(removed.files),
                      ui_locale::text(ui_locale::TextId::ImageFiles),
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
        draw_header(ui_locale::text(ui_locale::TextId::RssCache));

        char summary[80] = {};
        std::snprintf(summary, sizeof(summary), "%u %s, %s",
                      static_cast<unsigned>(_info.files),
                      ui_locale::text(ui_locale::TextId::ImageFiles),
                      format_cache_bytes(_info.bytes).c_str());
        system_font::draw_center(M5.Display, summary, width / 2, HEADER_H + 14, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::RssCacheRemoveHint),
                                 width / 2, 170, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::RssCacheKeepHint),
                                 width / 2, 230, TFT_BLACK);

        M5.Display.fillRoundRect(70, 340, width - 140, 100, 8, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ClearRssCache), width / 2,
                                 340 + std::max(0, (100 - system_font::line_height()) / 2),
                                 TFT_WHITE);
        if (!_message.empty()) {
            system_font::draw_center(M5.Display,
                                     system_font::ellipsize(_message, width - 50),
                                     width / 2, 500, TFT_BLACK);
        }

        if (_confirm) {
            M5.Display.fillRoundRect(35, 665, width - 70, 250, 10, TFT_WHITE);
            M5.Display.drawRoundRect(35, 665, width - 70, 250, 10, TFT_BLACK);
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::RssCacheConfirm),
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
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Cancel), left_x + button_w / 2,
                                     button_y + 20,
                                     _confirm_choice == 0 ? TFT_WHITE : TFT_BLACK);
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Clear), right_x + button_w / 2,
                                     button_y + 20,
                                     _confirm_choice == 1 ? TFT_WHITE : TFT_BLACK);
        }

        M5.Display.display();
        M5.Display.endWrite();
    }
};

class NetworkSettingsActivity final : public Activity {
public:
    void on_create() override
    {
        _network_mode = wifi_setting_get_network_mode();
        _power_mode = wifi_setting_get_power_mode();
        _focus = _network_mode == WIFI_SETTING_NETWORK_WIFI ? 0 : 1;
        draw(true);
    }

    void on_show() override
    {
        if (_shown) draw(false);
        _shown = true;
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        for (int i = 0; i < 5; ++i) {
            const int top = row_top(i);
            if (event.y >= top && event.y < top + 126) {
                _focus = i;
                select_focused();
                return true;
            }
        }
        return true;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            _focus = (_focus + (key == APP_KEY_DOWN ? 1 : 4)) % 5;
            draw(false);
            return true;
        }
        if (key == APP_KEY_OK) {
            select_focused();
            return true;
        }
        return false;
    }

private:
    wifi_setting_network_mode_t _network_mode = WIFI_SETTING_NETWORK_WIFI;
    wifi_setting_power_mode_t _power_mode = WIFI_SETTING_POWER_ON_DEMAND;
    int _focus = 0;
    bool _shown = false;

    static int row_top(int index) { return index < 2 ? 150 + index * 156
                                      : index < 4 ? 530 + (index - 2) * 156
                                                  : 895; }

    void select_focused()
    {
        if (_focus == 4) {
            ActivityManager::instance().push(new WifiSettingsActivity());
            return;
        }
        esp_err_t ret = ESP_OK;
        if (_focus < 2) {
            const wifi_setting_network_mode_t mode = _focus == 0
                ? WIFI_SETTING_NETWORK_WIFI : WIFI_SETTING_NETWORK_CELLULAR;
            ret = wifi_setting_set_network_mode(mode);
            if (ret == ESP_OK) _network_mode = mode;
        } else {
            const wifi_setting_power_mode_t mode = _focus == 2
                ? WIFI_SETTING_POWER_ALWAYS_ON : WIFI_SETTING_POWER_ON_DEMAND;
            ret = wifi_setting_set_power_mode(mode);
            _power_mode = wifi_setting_get_power_mode();
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "save network setting failed: %s", esp_err_to_name(ret));
            return;
        }
        draw(false);
    }

    void draw_option(int index, const char *title, const char *detail, bool active)
    {
        const int top = row_top(index);
        M5.Display.drawFastHLine(PAD, top, M5.Display.width() - PAD * 2, TFT_BLACK);
        if (_focus == index) {
            M5.Display.fillRect(PAD + 2, top + 20, 5, 88, TFT_BLACK);
        }
        system_font::draw_left(M5.Display, title, PAD + 18, top + 20, TFT_BLACK);
        system_font::draw_left(M5.Display, detail, PAD + 18, top + 68,
                               active ? TFT_BLACK : 0x8410);
        system_font::draw_right(M5.Display, active ? ui_locale::text(ui_locale::TextId::Selected) : "",
                                M5.Display.width() - PAD - 18, top + 20, TFT_BLACK);
        M5.Display.drawFastHLine(PAD, top + 125, M5.Display.width() - PAD * 2, TFT_BLACK);
    }

    void draw(bool quality)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::Network));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::NetworkChoiceHint),
                                 M5.Display.width() / 2, HEADER_H + 8, 0x8410);
        draw_option(0, "WiFi", ui_locale::text(ui_locale::TextId::WifiChoiceHint),
                    _network_mode == WIFI_SETTING_NETWORK_WIFI);
        draw_option(1, "4G", ui_locale::text(ui_locale::TextId::CellularChoiceHint),
                    _network_mode == WIFI_SETTING_NETWORK_CELLULAR);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::WifiPowerMode),
                                 M5.Display.width() / 2, 465, 0x8410);
        draw_option(2, ui_locale::text(ui_locale::TextId::WifiAlwaysOn),
                    ui_locale::text(ui_locale::TextId::WifiAlwaysOnHint),
                    _power_mode == WIFI_SETTING_POWER_ALWAYS_ON);
        draw_option(3, ui_locale::text(ui_locale::TextId::WifiOnDemand),
                    ui_locale::text(ui_locale::TextId::WifiOnDemandHint),
                    _power_mode == WIFI_SETTING_POWER_ON_DEMAND);
        char ssid[WIFI_SETTING_SSID_MAX_LEN + 1] = {};
        char password[WIFI_SETTING_PASSWORD_MAX_LEN + 1] = {};
        const bool configured = wifi_setting_load_credentials(ssid, sizeof(ssid),
                                                               password, sizeof(password));
        const int top = row_top(4);
        M5.Display.drawFastHLine(PAD, top, M5.Display.width() - PAD * 2, TFT_BLACK);
        M5.Display.drawFastHLine(PAD, top + 125, M5.Display.width() - PAD * 2, TFT_BLACK);
        if (_focus == 4) M5.Display.fillRect(PAD + 2, top + 20, 5, 88, TFT_BLACK);
        system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::Wifi),
                               PAD + 18, top + 20, TFT_BLACK);
        const std::string detail = configured
            ? std::string(ui_locale::text(ui_locale::TextId::DefaultNetwork)) + ssid
            : ui_locale::text(ui_locale::TextId::NotConfigured);
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(detail, M5.Display.width() - PAD * 2 - 80),
                               PAD + 18, top + 68, TFT_DARKGRAY);
        system_font::draw_right(M5.Display, ">", M5.Display.width() - PAD - 18,
                                top + 20, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class FrontlightSettingsActivity final : public Activity {
public:
    void on_create() override { draw(true); }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        const Layout layout = make_layout(M5.Display.width());
        if (event.y >= CONTROL_Y && event.y < CONTROL_Y + CONTROL_H) {
            if (event.x >= layout.minus_x && event.x < layout.minus_x + layout.button_w) change(-1);
            else if (event.x >= layout.plus_x && event.x < layout.plus_x + layout.button_w) change(1);
            else if (event.x >= layout.bar_x && event.x < layout.bar_x + layout.bar_area_w) {
                const int level = std::clamp(
                    ((event.x - layout.bar_x) * 10 + layout.bar_area_w - 1)
                        / layout.bar_area_w,
                    0, 10);
                frontlight_set_level(level);
                draw(false);
            }
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
        if (key == APP_KEY_UP) { change(1); return true; }
        if (key == APP_KEY_DOWN) { change(-1); return true; }
        return false;
    }

private:
    static constexpr int CONTROL_Y = 350;
    static constexpr int CONTROL_H = 132;

    struct Layout {
        int minus_x;
        int plus_x;
        int button_w;
        int bar_x;
        int bar_area_w;
        int bar_w;
        int bar_gap;
    };

    static Layout make_layout(int width)
    {
        constexpr int margin = 20;
        constexpr int side_gap = 12;
        constexpr int bar_gap = 6;
        // The page is portrait (684 px wide).  Keep the - / bars / + groups
        // within that live width rather than using the panel's 1216 px axis.
        const int button_w = std::clamp(width / 8, 72, 88);
        const int minus_x = margin;
        const int plus_x = width - margin - button_w;
        const int bar_x = minus_x + button_w + side_gap;
        const int bar_area_w = std::max(10, plus_x - side_gap - bar_x);
        const int bar_w = std::max(4, (bar_area_w - 9 * bar_gap) / 10);
        return {minus_x, plus_x, button_w, bar_x, bar_area_w, bar_w, bar_gap};
    }

    void change(int delta)
    {
        frontlight_set_level(std::clamp(frontlight_get_level() + delta, 0, 10));
        draw(false);
    }

    void draw(bool quality)
    {
        const int level = frontlight_get_level();
        const int width = M5.Display.width();
        const Layout layout = make_layout(width);
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::Frontlight));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Brightness), width / 2, HEADER_H + 14, TFT_BLACK);
        system_font::draw_center(M5.Display, std::to_string(frontlight_get_percent()) + "%",
                                 width / 2, 180, TFT_BLACK);

        M5.Display.drawRoundRect(layout.minus_x, CONTROL_Y, layout.button_w, CONTROL_H,
                                 10, TFT_BLACK);
        M5.Display.drawRoundRect(layout.plus_x, CONTROL_Y, layout.button_w, CONTROL_H,
                                 10, TFT_BLACK);
        system_font::draw_center(M5.Display, "-", layout.minus_x + layout.button_w / 2,
                                 CONTROL_Y + 38, TFT_BLACK);
        system_font::draw_center(M5.Display, "+", layout.plus_x + layout.button_w / 2,
                                 CONTROL_Y + 38, TFT_BLACK);
        for (int i = 0; i < 10; ++i) {
            const int x = layout.bar_x + i * (layout.bar_w + layout.bar_gap);
            const bool on = i < level;
            if (on) M5.Display.fillRoundRect(x, CONTROL_Y + 43, layout.bar_w, 46, 5, TFT_BLACK);
            else M5.Display.drawRoundRect(x, CONTROL_Y + 43, layout.bar_w, 46, 5, TFT_BLACK);
        }
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::BrightnessStepHint),
                                 width / 2, 590, TFT_DARKGRAY);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::FrontlightOffHint),
                                 width / 2, 646, TFT_DARKGRAY);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

std::string time_basis_label()
{
    const int offset = time_basis_get_offset_minutes();
    char label[32];
    std::snprintf(label, sizeof(label), "UTC%c%02d:%02d%s", offset < 0 ? '-' : '+',
                  std::abs(offset) / 60, std::abs(offset) % 60,
                  offset == 480 ? "  " : "");
    if (offset == 480) return std::string(label) + ui_locale::text(ui_locale::TextId::Beijing);
    return label;
}

class TimeBasisSettingsActivity final : public Activity {
public:
    void on_create() override { draw(true); }
    void on_show() override
    {
        if (_shown) draw(false);
        _shown = true;
    }
    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::Tap && event.y >= 250 && event.y < 380) {
            change(event.x < M5.Display.width() / 2 ? -30 : 30);
            return true;
        }
        if (event.type == TouchEventType::Tap && event.y >= 610 && event.y < 760) {
            change_clock_basis();
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
            change(key == APP_KEY_UP ? 30 : -30);
            return true;
        }
        if (key == APP_KEY_OK) {
            change_clock_basis();
            return true;
        }
        return false;
    }
private:
    bool _shown = false;
    std::string _error;
    void change(int delta)
    {
        const int proposed = std::clamp(time_basis_get_offset_minutes() + delta,
                                        -12 * 60, 14 * 60);
        const esp_err_t ret = time_basis_set_offset_minutes(proposed);
        _error = ret == ESP_OK ? "" : esp_err_to_name(ret);
        draw(false);
    }
    void change_clock_basis()
    {
        const esp_err_t ret = time_basis_set_modem_clock_is_utc(
            !time_basis_modem_clock_is_utc());
        _error = ret == ESP_OK ? "" : esp_err_to_name(ret);
        draw(false);
    }
    void draw(bool quality)
    {
        const int width = M5.Display.width();
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::TimeBasis));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::PhoneSmsTimezone),
                                 width / 2, HEADER_H + 14, TFT_BLACK);
        system_font::draw_center(M5.Display, time_basis_label(), width / 2, 185,
                                 TFT_BLACK);
        const int button_width = (width - 120) / 2;
        M5.Display.drawRect(40, 250, button_width, 130, TFT_BLACK);
        M5.Display.drawRect(80 + button_width, 250, button_width, 130, TFT_BLACK);
        system_font::draw_center(M5.Display, "- 30 min", 40 + button_width / 2,
                                 285, TFT_BLACK);
        system_font::draw_center(M5.Display, "+ 30 min", 80 + button_width + button_width / 2,
                                 285, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::BeijingDefaultHint),
                                 width / 2, 440, TFT_DARKGRAY);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ModemTimeHint),
                                 width / 2, 505, TFT_DARKGRAY);
        M5.Display.drawRect(40, 610, width - 80, 150, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ModemClockDigits),
                                 width / 2, 630, TFT_BLACK);
        system_font::draw_center(M5.Display,
                                 time_basis_modem_clock_is_utc()
                                     ? ui_locale::text(ui_locale::TextId::UtcIgnoreReportedZone)
                                     : ui_locale::text(ui_locale::TextId::UseReportedZone),
                                 width / 2, 685, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::SmsOwnZoneHint),
                                 width / 2, 810, TFT_DARKGRAY);
        if (!_error.empty()) system_font::draw_center(M5.Display, _error, width / 2,
                                                       880, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class DateTimeSettingsActivity final : public Activity {
public:
    void on_create() override { draw(true); }
    void on_show() override
    {
        if (_shown) draw(false);
        _shown = true;
    }
    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap || event.y < ROW_Y ||
            event.y >= ROW_Y + ROW_H * 3) return false;
        _focus = (event.y - ROW_Y) / ROW_H;
        activate();
        return true;
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
            activate();
            return true;
        }
        return false;
    }
private:
    static constexpr int ROW_Y = HEADER_H;
    static constexpr int ROW_H = 210;
    int _focus = 0;
    bool _shown = false;
    std::string _status;

    void activate()
    {
        if (_focus == 0) sync_network_time();
        else if (_focus == 1) {
            esp_err_t ret = time_basis_set_use_24_hour(!time_basis_use_24_hour());
            _status = ret == ESP_OK ? "" : esp_err_to_name(ret);
            draw(false);
        } else {
            ActivityManager::instance().push(new TimeBasisSettingsActivity());
        }
    }

    void sync_network_time()
    {
        using ui_locale::TextId;
        _status = ui_locale::text(TextId::SyncingTime);
        draw(false);
        const bool was_connected = wifi_setting_is_connected();
        esp_err_t ret = was_connected ? ESP_OK : wifi_setting_connect_saved(15000);
        if (ret == ESP_OK) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
            ret = esp_netif_sntp_init(&config);
            if (ret == ESP_OK) {
                ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
                esp_netif_sntp_deinit();
            }
            if (ret == ESP_OK && !time_basis_system_clock_valid()) ret = ESP_ERR_INVALID_RESPONSE;
        }
        if (!was_connected && wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_WIFI) {
            wifi_setting_release();
        }
        _status = ret == ESP_OK ? ui_locale::text(TextId::TimeSyncSuccess)
                                : std::string(ui_locale::text(TextId::TimeSyncFailed)) +
                                      ": " + esp_err_to_name(ret);
        ESP_LOGI(TAG, "network time sync: %s unix=%lld", esp_err_to_name(ret),
                 static_cast<long long>(std::time(nullptr)));
        draw(false);
    }

    void draw(bool quality)
    {
        using ui_locale::TextId;
        const int width = M5.Display.width();
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(TextId::DateAndTime));
        char clock[24] = {};
        char datetime[32] = {};
        const std::time_t now = std::time(nullptr);
        if (time_basis_system_clock_valid()) {
            time_basis_format_clock(now, clock, sizeof(clock));
            time_basis_format_utc(now, datetime, sizeof(datetime), nullptr);
        }
        const char *titles[] = {ui_locale::text(TextId::SyncTime),
                                ui_locale::text(TextId::HourFormat),
                                ui_locale::text(TextId::TimeBasis)};
        const std::string current_time = time_basis_system_clock_valid()
            ? std::string(datetime, 10) + " " + clock
            : ui_locale::text(TextId::TimeNotSet);
        const std::string details[] = {
            current_time,
            time_basis_use_24_hour() ? "24 h" : "12 h",
            time_basis_label(),
        };
        for (int i = 0; i < 3; ++i) {
            const int top = ROW_Y + i * ROW_H;
            if (i == 0) M5.Display.drawFastHLine(PAD, top, width - 2 * PAD, TFT_BLACK);
            M5.Display.drawFastHLine(PAD, top + ROW_H - 1, width - 2 * PAD, TFT_BLACK);
            if (i == _focus) M5.Display.fillRect(PAD + 2, top + 22, 5, ROW_H - 44, TFT_BLACK);
            system_font::draw_left(M5.Display, titles[i], PAD + 20, top + 30, TFT_BLACK);
            system_font::draw_left(M5.Display, details[i], PAD + 20, top + 95, TFT_DARKGRAY);
            if (i == 2) system_font::draw_right(M5.Display, ">", width - PAD - 20,
                                                 top + 58, TFT_BLACK);
        }
        if (!_status.empty()) system_font::draw_center(M5.Display, _status,
                                                        width / 2, 860, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class LanguageSettingsActivity final : public Activity {
public:
    void on_create() override { draw(); }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap) return false;
        constexpr int row_y = HEADER_H;
        constexpr int row_h = 170;
        if (event.y < row_y || event.y >= row_y + row_h * 3) return false;
        choose((event.y - row_y) / row_h);
        return true;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            _focus = (_focus + (key == APP_KEY_DOWN ? 1 : 2)) % 3;
            draw();
            return true;
        }
        if (key == APP_KEY_OK) {
            choose(_focus);
            return true;
        }
        return false;
    }

private:
    int _focus = static_cast<int>(ui_locale::get());

    void choose(int index)
    {
        auto language = static_cast<ui_locale::Language>(index);
        esp_err_t result = ui_locale::set(language);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "save UI language failed: %s", esp_err_to_name(result));
            draw();
            return;
        }
        ActivityManager::instance().pop();
    }

    void draw()
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::SelectLanguage));
        constexpr int row_y = HEADER_H;
        constexpr int row_h = 170;
        for (int i = 0; i < 3; ++i) {
            int y = row_y + i * row_h;
            const char *name = ui_locale::native_name(static_cast<ui_locale::Language>(i));
            M5.Display.drawFastHLine(PAD, y + row_h - 1,
                                    M5.Display.width() - PAD * 2, TFT_BLACK);
            if (i == _focus) M5.Display.fillRect(PAD + 2, y + 35, 5, row_h - 70, TFT_BLACK);
            system_font::draw_left(M5.Display, name, PAD + 25, y + 48, TFT_BLACK);
            if (i == static_cast<int>(ui_locale::get())) {
                system_font::draw_right(M5.Display, "*", M5.Display.width() - PAD - 20,
                                        y + 48, TFT_BLACK);
            }
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class AppSettingsActivity final : public Activity {
public:
    void on_create() override { draw(true); }
    void on_show() override
    {
        if (_shown) draw(false);
        _shown = true;
    }
    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type != TouchEventType::Tap || event.y < ROW_Y ||
            event.y >= ROW_Y + ROW_H * 3) return false;
        _focus = (event.y - ROW_Y) / ROW_H;
        open_focused();
        return true;
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
            open_focused();
            return true;
        }
        return false;
    }
private:
    static constexpr int ROW_Y = HEADER_H;
    static constexpr int ROW_H = 210;
    int _focus = 0;
    bool _shown = false;

    void open_focused()
    {
        if (_focus == 0) ActivityManager::instance().push(new DeepSeekSettingsActivity());
        else if (_focus == 1) ActivityManager::instance().push(new ZhihuSettingsActivity());
        else ActivityManager::instance().push(new MapSettingsActivity());
    }

    void draw(bool quality)
    {
        using ui_locale::TextId;
        using ui_locale::text;
        const int width = M5.Display.width();
        DeepSeekConfig deepseek;
        const bool deepseek_configured = deepseek_config_load(&deepseek);
        ZhihuConfig zhihu;
        const bool zhihu_configured = zhihu_config_load(&zhihu);
        AmapConfig amap;
        const bool map_configured = amap_config_load(&amap);
        const char *titles[] = {text(TextId::DeepSeek), text(TextId::Zhihu), text(TextId::Map)};
        const std::string details[] = {
            deepseek_configured
                ? std::string(text(TextId::ApiKeyConfigured)) + ", " +
                      deepseek_model_label(deepseek.default_model)
                : text(TextId::ApiKeyNotConfigured),
            text(zhihu_configured ? TextId::AccessSecretConfigured
                                  : TextId::AccessSecretNotConfigured),
            text(map_configured ? TextId::ApiKeyConfigured : TextId::ApiKeyNotConfigured),
        };
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(text(TextId::AppSettings));
        for (int i = 0; i < 3; ++i) {
            const int top = ROW_Y + i * ROW_H;
            if (i == 0) M5.Display.drawFastHLine(PAD, top, width - 2 * PAD, TFT_BLACK);
            M5.Display.drawFastHLine(PAD, top + ROW_H - 1, width - 2 * PAD, TFT_BLACK);
            if (i == _focus) M5.Display.fillRect(PAD + 2, top + 22, 5, ROW_H - 44, TFT_BLACK);
            system_font::draw_left(M5.Display, titles[i], PAD + 20, top + 30, TFT_BLACK);
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(details[i], width - 2 * PAD - 80),
                                   PAD + 20, top + 95, TFT_DARKGRAY);
            system_font::draw_right(M5.Display, ">", width - PAD - 20,
                                    top + 58, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class SettingActivity final : public Activity {
public:
    void on_create() override
    {
        _seen_generation = ui_locale::generation();
        draw(true);
    }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        const bool changed = _seen_generation != ui_locale::generation();
        _seen_generation = ui_locale::generation();
        draw(changed);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::Tap) {
            for (int row = 0; row < SETTINGS_VISIBLE_ROWS; ++row) {
                int i = _first_visible + row;
                if (i >= SETTINGS_ITEM_COUNT) break;
                int top = SETTINGS_ROW_Y + row * (SETTINGS_ROW_H + SETTINGS_ROW_GAP);
                if (event.y >= top && event.y < top + SETTINGS_ROW_H) {
                    _focus = i;
                    open_focused();
                    return true;
                }
            }
        }
        if (event.type == TouchEventType::SwipeUp &&
            _first_visible < SETTINGS_ITEM_COUNT - SETTINGS_VISIBLE_ROWS) {
            ++_first_visible;
            _focus = std::max(_focus, _first_visible);
            draw(false);
            return true;
        }
        if (event.type == TouchEventType::SwipeDown && _first_visible > 0) {
            --_first_visible;
            _focus = std::min(_focus, _first_visible + SETTINGS_VISIBLE_ROWS - 1);
            draw(false);
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
        if (key == APP_KEY_UP) {
            _focus = (_focus + SETTINGS_ITEM_COUNT - 1) % SETTINGS_ITEM_COUNT;
            ensure_focus_visible();
            draw(false);
            return true;
        }
        if (key == APP_KEY_DOWN) {
            _focus = (_focus + 1) % SETTINGS_ITEM_COUNT;
            ensure_focus_visible();
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
    int _first_visible = 0;
    uint32_t _seen_generation = 0;

    void ensure_focus_visible()
    {
        if (_focus < _first_visible) _first_visible = _focus;
        if (_focus >= _first_visible + SETTINGS_VISIBLE_ROWS) {
            _first_visible = _focus - SETTINGS_VISIBLE_ROWS + 1;
        }
    }

    void open_focused()
    {
        if (_focus == 0) ActivityManager::instance().push(new LanguageSettingsActivity());
        else if (_focus == 1) ActivityManager::instance().push(new NetworkSettingsActivity());
        else if (_focus == 2) ActivityManager::instance().push(new FrontlightSettingsActivity());
        else if (_focus == 3) ActivityManager::instance().push(new AppSettingsActivity());
        else if (_focus == 4) launch_epd_diagnostics();
        else if (_focus == 5) ActivityManager::instance().push(new RssCacheSettingsActivity());
        else if (_focus == 6) ActivityManager::instance().push(new DateTimeSettingsActivity());
        else {
            const bool enabled = !home_icon_frames_enabled();
            const esp_err_t result = home_set_icon_frames_enabled(enabled);
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "save home icon frame preference failed: %s",
                         esp_err_to_name(result));
            }
            draw(false);
        }
    }

    void draw_item(int index, const char *title, const std::string &detail,
                   bool navigable = true)
    {
        const int width = M5.Display.width();
        if (index < _first_visible || index >= _first_visible + SETTINGS_VISIBLE_ROWS) return;
        const int top = SETTINGS_ROW_Y + (index - _first_visible) * (SETTINGS_ROW_H + SETTINGS_ROW_GAP);
        const bool selected = index == _focus;
        if (index == _first_visible) {
            M5.Display.drawFastHLine(PAD, top, width - PAD * 2, TFT_BLACK);
        }
        M5.Display.drawFastHLine(PAD, top + SETTINGS_ROW_H - 1,
                                width - PAD * 2, TFT_BLACK);
        if (selected) {
            M5.Display.fillRect(PAD + 2, top + 16, 5,
                                SETTINGS_ROW_H - 32, TFT_BLACK);
        }
        system_font::draw_left(M5.Display, title, PAD + 18,
                               top + 10, TFT_BLACK);
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(detail, width - PAD * 2 - 80),
                               PAD + 18, top + 62, TFT_BLACK);
        if (navigable) {
            system_font::draw_right(M5.Display, ">", width - PAD - 20,
                                    top + 36, TFT_BLACK);
        }
    }

    void draw(bool quality)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        using ui_locale::TextId;
        using ui_locale::text;
        draw_header(text(TextId::Settings));
        const wifi_setting_network_mode_t network_mode = wifi_setting_get_network_mode();
        draw_item(0, text(TextId::Language), ui_locale::native_name(ui_locale::get()));
        draw_item(1, text(TextId::Network), network_mode == WIFI_SETTING_NETWORK_CELLULAR
                                      ? text(TextId::CellularData)
                                      : std::string(text(TextId::Wifi)) + " / " +
                                            text(wifi_setting_get_power_mode() == WIFI_SETTING_POWER_ALWAYS_ON
                                                 ? TextId::WifiAlwaysOn : TextId::WifiOnDemand));
        draw_item(2, text(TextId::Frontlight), std::to_string(frontlight_get_percent()) + "%");
        draw_item(3, text(TextId::AppSettings),
                  std::string(text(TextId::DeepSeek)) + " / " + text(TextId::Zhihu) +
                      " / " + text(TextId::Map));
        draw_item(4, text(TextId::EpdDiagnostics), text(TextId::EpdDiagnosticsDetail));
        RssCacheInfo rss_cache = rss_cache_info();
        char rss_detail[64] = {};
        std::snprintf(rss_detail, sizeof(rss_detail), "%u %s, %s",
                      static_cast<unsigned>(rss_cache.files),
                      text(TextId::FilesUnit),
                      format_cache_bytes(rss_cache.bytes).c_str());
        draw_item(5, text(TextId::ClearRssCache), rss_detail);
        draw_item(6, text(TextId::DateAndTime), time_basis_label());
        draw_item(7, text(TextId::HomeIconFrames),
                  home_icon_frames_enabled() ? text(TextId::On) : text(TextId::Off), false);
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
        draw_header(ui_locale::text(ui_locale::TextId::WifiRequired));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::WifiNotConfigured),
                                 M5.Display.width() / 2, 350, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::OpenWifiSettingsHint),
                                 M5.Display.width() / 2, 440, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Returning),
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
    if (wifi_setting_get_network_mode() == WIFI_SETTING_NETWORK_CELLULAR) {
        return true;
    }
    char ssid[WIFI_SETTING_SSID_MAX_LEN + 1] = {};
    char password[WIFI_SETTING_PASSWORD_MAX_LEN + 1] = {};
    if (wifi_setting_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        return true;
    }
    ActivityManager::instance().push(new WifiSetupGuideActivity());
    return false;
}
