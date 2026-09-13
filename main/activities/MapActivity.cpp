#include "activities/MapActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/SettingActivity.h"
#include "map/AmapClient.h"
#include "map/AmapConfig.h"
#include "map/AmapImageProcessor.h"
#include "renderer/P4JpegDecoder.h"
#include "renderer/TurboJpegDecoder.h"
#include "storage/app_paths.h"
#include "ui/SystemFont.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
#include <esp_err.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

constexpr const char *TAG = "MAP";
constexpr int HEADER_H = 76;
constexpr int CONTROL_X_PAD = 18;
constexpr int CONTROL_W = 88;
constexpr int CONTROL_H = 82;
constexpr int CONTROL_GAP = 12;
constexpr int CONTROL_TOP = 110;
constexpr int STATUS_H = 70;
constexpr double MERCATOR_MAX_LATITUDE = 85.051129;
constexpr double PI = 3.14159265358979323846;

bool file_exists(const char *path)
{
    if (!path || !path[0]) return false;
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

class MapActivity final : public Activity {
public:
    ~MapActivity() override
    {
        wifi_setting_deinit(true);
    }

    void on_create() override
    {
        amap_config_load(&_config);
        _map_format = AmapClient::probe_image(app_paths::map_image_file());
        _has_map = _map_format != AmapImageFormat::Unknown;
        if (_has_map) {
            draw_map(true, "Cached map");
        } else {
            draw_status("Loading map...", true);
        }
        M5.Display.waitDisplay();

        if (_config.api_key.empty()) {
            _message = "Set AMap API Key in Settings";
            draw_current(false);
            return;
        }
        if (!connect_wifi()) {
            draw_current(false);
            return;
        }
        if (!_has_map) reload_map();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && event.y < HEADER_H && event.x < 120)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::Tap) {
            const int x = M5.Display.width() - CONTROL_X_PAD - CONTROL_W;
            if (event.x >= x && event.y >= CONTROL_TOP &&
                event.y < CONTROL_TOP + CONTROL_H) {
                change_zoom(1);
                return true;
            }
            const int minus_y = CONTROL_TOP + CONTROL_H + CONTROL_GAP;
            if (event.x >= x && event.y >= minus_y && event.y < minus_y + CONTROL_H) {
                change_zoom(-1);
                return true;
            }
            if (!_has_map || !_message.empty()) {
                reload_map();
                return true;
            }
        }
        if (event.type == TouchEventType::SwipeLeft ||
            event.type == TouchEventType::SwipeRight ||
            event.type == TouchEventType::SwipeUp ||
            event.type == TouchEventType::SwipeDown) {
            pan(event);
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
            change_zoom(1);
            return true;
        }
        if (key == APP_KEY_DOWN) {
            change_zoom(-1);
            return true;
        }
        if (key == APP_KEY_OK || key == APP_KEY_EXTRA) {
            reload_map();
            return true;
        }
        return false;
    }

private:
    AmapConfig _config;
    AmapClient _client;
    AmapGrayFrame _gray_frame;
    AmapImageFormat _map_format = AmapImageFormat::Unknown;
    bool _has_map = false;
    std::string _message;

    bool connect_wifi()
    {
        if (wifi_setting_is_connected()) return true;
        _message = "Connecting WiFi...";
        draw_current(false);
        M5.Display.waitDisplay();
        const esp_err_t error =
            wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
        if (error == ESP_OK) {
            _message.clear();
            ESP_LOGI(TAG, "WiFi ready for map session");
            return true;
        }
        _message = std::string("WiFi failed: ") + esp_err_to_name(error);
        ESP_LOGW(TAG, "WiFi connection failed: %s", esp_err_to_name(error));
        return false;
    }

    void change_zoom(int delta)
    {
        const int target = std::max(1, std::min(17, static_cast<int>(_config.zoom) + delta));
        if (target == _config.zoom) return;
        const AmapConfig old = _config;
        _config.zoom = static_cast<uint8_t>(target);
        if (!reload_map()) _config = old;
    }

    void pan(const TouchEvent &event)
    {
        int dx = event.dx;
        int dy = event.dy;
        if (dx == 0 && event.type == TouchEventType::SwipeLeft) dx = -M5.Display.width() / 3;
        if (dx == 0 && event.type == TouchEventType::SwipeRight) dx = M5.Display.width() / 3;
        if (dy == 0 && event.type == TouchEventType::SwipeUp) dy = -M5.Display.height() / 3;
        if (dy == 0 && event.type == TouchEventType::SwipeDown) dy = M5.Display.height() / 3;

        const double world_size = 256.0 * std::pow(2.0, static_cast<int>(_config.zoom));
        double world_x = (_config.longitude + 180.0) / 360.0 * world_size;
        const double latitude = std::max(-MERCATOR_MAX_LATITUDE,
                                         std::min(MERCATOR_MAX_LATITUDE, _config.latitude));
        const double radians = latitude * PI / 180.0;
        double world_y = (1.0 - std::log(std::tan(radians) + 1.0 / std::cos(radians)) /
                                  PI) * 0.5 * world_size;
        // scale=2 doubles response pixels while preserving the logical viewport.
        world_x -= static_cast<double>(dx) / 2.0;
        world_y -= static_cast<double>(dy) / 2.0;
        world_x = std::fmod(world_x, world_size);
        if (world_x < 0.0) world_x += world_size;
        world_y = std::max(0.0, std::min(world_size, world_y));

        const double longitude = world_x / world_size * 360.0 - 180.0;
        const double mercator = PI * (1.0 - 2.0 * world_y / world_size);
        const double new_latitude = std::atan(std::sinh(mercator)) * 180.0 / PI;
        const AmapConfig old = _config;
        _config.longitude = longitude;
        _config.latitude = new_latitude;
        if (!reload_map()) _config = old;
    }

    bool reload_map()
    {
        if (_config.api_key.empty()) {
            _message = "Set AMap API Key in Settings";
            draw_current(false);
            return false;
        }
        M5.Display.waitDisplay();
        if (!connect_wifi()) {
            draw_current(false);
            return false;
        }
        _message = "Loading map...";
        draw_current(false);
        M5.Display.waitDisplay();
        AmapMapResult result = _client.fetch_static_map(
            _config, M5.Display.width(), M5.Display.height());
        // The AMap static-map endpoint currently replies with
        // `Connection: close`, so retaining the HTTP/TLS client cannot keep
        // the socket alive. Release all network-side buffers before PNG
        // decoding and the memory-sensitive GC16 refresh pipeline start.
        _client.close();
        if (!result.success) {
            _message = result.error;
            draw_current(false);
            return false;
        }
        _map_format = result.format;
        _has_map = true;
        _gray_frame.reset();
        _message.clear();
        amap_config_save_viewport(_config.longitude, _config.latitude, _config.zoom);
        draw_map(true, "");
        return true;
    }

    bool draw_image()
    {
        const char *path = app_paths::map_image_file();
        if (!file_exists(path)) return false;
        if (_map_format == AmapImageFormat::Png) {
#if MYPAPER_AMAP_LOW_RES_PIPELINE
            if (!_gray_frame.valid() &&
                !_gray_frame.load_png(path,
                                      static_cast<uint16_t>(M5.Display.width()),
                                      static_cast<uint16_t>(M5.Display.height()))) {
                ESP_LOGW(TAG, "processed map unavailable; using direct PNG fallback");
            }
            if (_gray_frame.valid()) {
                auto *panel = M5.Display.getPanel();
                if (panel && panel->writeNativeGrayscale4(
                                 _gray_frame.data(),
                                 _gray_frame.width(),
                                 _gray_frame.height())) {
                    return true;
                }
                ESP_LOGW(TAG, "native map import unavailable; using direct PNG fallback");
            }
#endif
#if MYPAPER_AMAP_LOW_RES_PIPELINE
            const float fallback_zoom = _gray_frame.valid()
                                            ? static_cast<float>(_gray_frame.nearest_scale())
                                            : 2.0f;
            return M5.Display.drawPngFile(path, 0, 0,
                                          M5.Display.width(), M5.Display.height(),
                                          0, 0, fallback_zoom, fallback_zoom);
#else
            return M5.Display.drawPngFile(path, 0, 0);
#endif
        }
        if (_map_format == AmapImageFormat::Jpeg) {
            p4_jpeg::DrawStats hardware;
            if (p4_jpeg::draw_file(M5.Display, path, 0, 0,
                                   M5.Display.width(), M5.Display.height(), &hardware)) {
                return true;
            }
            if (M5.Display.drawJpgFile(path, 0, 0)) return true;
            turbo_jpeg::DrawStats software;
            return turbo_jpeg::draw_file(M5.Display, path, 0, 0,
                                         M5.Display.width(), M5.Display.height(), &software);
        }
        return false;
    }

    void draw_header()
    {
        const int width = M5.Display.width();
        M5.Display.fillRect(0, 0, width, HEADER_H, TFT_WHITE);
        system_font::draw_left(M5.Display, "<", 18, 12, TFT_BLACK);
        system_font::draw_center(M5.Display, "Map", width / 2, 12, TFT_BLACK);
        M5.Display.drawFastHLine(0, HEADER_H - 1, width, TFT_BLACK);
    }

    void draw_controls()
    {
        const int x = M5.Display.width() - CONTROL_X_PAD - CONTROL_W;
        const int minus_y = CONTROL_TOP + CONTROL_H + CONTROL_GAP;
        M5.Display.fillRoundRect(x, CONTROL_TOP, CONTROL_W, CONTROL_H, 8, TFT_WHITE);
        M5.Display.drawRoundRect(x, CONTROL_TOP, CONTROL_W, CONTROL_H, 8, TFT_BLACK);
        system_font::draw_center(M5.Display, "+", x + CONTROL_W / 2,
                                 CONTROL_TOP + 13, TFT_BLACK);
        M5.Display.fillRoundRect(x, minus_y, CONTROL_W, CONTROL_H, 8, TFT_WHITE);
        M5.Display.drawRoundRect(x, minus_y, CONTROL_W, CONTROL_H, 8, TFT_BLACK);
        system_font::draw_center(M5.Display, "-", x + CONTROL_W / 2,
                                 minus_y + 13, TFT_BLACK);
    }

    void draw_status_bar(const std::string &message)
    {
        const int width = M5.Display.width();
        const int y = M5.Display.height() - STATUS_H;
        M5.Display.fillRect(0, y, width, STATUS_H, TFT_WHITE);
        M5.Display.drawFastHLine(0, y, width, TFT_BLACK);
        char position[96] = {};
        if (message.empty()) {
            std::snprintf(position, sizeof(position), "%.5f, %.5f    Z%u",
                          _config.longitude, _config.latitude,
                          static_cast<unsigned>(_config.zoom));
        }
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(message.empty() ? position : message,
                                                        width - 30),
                                 width / 2, y + 11, TFT_BLACK);
    }

    void draw_map(bool quality, const std::string &message)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        if (!draw_image()) {
            _has_map = false;
            system_font::draw_center(M5.Display, "Map image decode failed",
                                     M5.Display.width() / 2,
                                     M5.Display.height() / 2, TFT_BLACK);
        }
        draw_header();
        draw_controls();
        draw_status_bar(message);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_status(const std::string &message, bool quality)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();
        system_font::draw_center(M5.Display, system_font::ellipsize(message,
                                 M5.Display.width() - 40), M5.Display.width() / 2,
                                 M5.Display.height() / 2 - 20, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_current(bool quality)
    {
        if (_has_map) draw_map(quality, _message);
        else draw_status(_message.empty() ? "Tap to load map" : _message, quality);
    }
};

} // namespace

void launch_map()
{
    if (!ensure_wifi_configured_or_show_guide()) return;
    ActivityManager::instance().push(new MapActivity());
}
