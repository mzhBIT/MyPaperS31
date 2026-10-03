#include "activities/MapActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/SettingActivity.h"
#include "activities/MapSearchActivity.h"
#include "activities/MapRouteActivity.h"
#include "map/MapCoordinates.h"
#include "map/AmapClient.h"
#include "map/AmapConfig.h"
#include "map/AmapImageProcessor.h"
#include "map/OfflineMapSource.h"
#include "map/MapSourcePolicy.h"
#include "map/MapScale.h"
#include "map/MapCache.h"
#include <esp_timer.h>
#include "renderer/P4JpegDecoder.h"
#include "renderer/TurboJpegDecoder.h"
#include "storage/app_paths.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"
#include "wifi/wifi_setting.h"

#include <M5Unified.h>
#include <esp_err.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <memory>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

constexpr const char *TAG = "MAP";
constexpr int HEADER_H = header_layout::height;
constexpr int CONTROL_X_PAD = 18;
constexpr int CONTROL_W = 88;
constexpr int CONTROL_H = 82;
constexpr int CONTROL_GAP = 12;
constexpr int CONTROL_TOP = 110;
constexpr int STATUS_H = 0; // No bottom status strip; the map uses the full height.
constexpr double MERCATOR_MAX_LATITUDE = 85.051129;
constexpr double PI = 3.14159265358979323846;

struct MapRequest {
    AmapConfig config;
    int width = 0, height = 0;
    uint32_t generation = 0;
    std::atomic<bool> cancel{false}, done{false};
    AmapMapResult result;
};

void map_fetch_task(void *arg)
{
    std::unique_ptr<std::shared_ptr<MapRequest>> holder(static_cast<std::shared_ptr<MapRequest> *>(arg));
    auto request = *holder;
    {
        AmapClient client;
        const esp_err_t error = request->cancel.load() ? ESP_ERR_INVALID_STATE
            : wifi_setting_connect_saved(CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS);
        if (error == ESP_OK && !request->cancel.load()) {
            request->result = client.fetch_static_map(request->config, request->width,
                                                      request->height, &request->cancel);
        } else request->result.error = esp_err_to_name(error);
        // Release TLS before publishing completion and allowing Quality refresh.
        client.close();
    }
    request->done.store(true, std::memory_order_release);
    request.reset();
    holder.reset();
    vTaskDeleteWithCaps(nullptr);
}

bool file_exists(const char *path)
{
    if (!path || !path[0]) return false;
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

std::string localized_map_error(const std::string &error)
{
    using ui_locale::TextId;
    struct ErrorText { const char *source; TextId id; };
    static constexpr ErrorText exact[] = {
        {"Network time synchronization failed", TextId::MapTimeSyncFailed},
        {"AMap returned an invalid image", TextId::MapInvalidImage},
        {"AMap API key is invalid or expired", TextId::MapKeyInvalid},
        {"AMap daily request limit reached", TextId::MapDailyLimit},
        {"AMap request rate limit reached", TextId::MapRateLimit},
        {"AMap API key is not configured", TextId::MapKeyMissing},
        {"Failed to create map cache file", TextId::MapCacheCreateFailed},
        {"Failed to create AMap HTTPS client", TextId::MapHttpsInitFailed},
        {"AMap image is too large", TextId::MapImageTooLarge},
        {"Failed while downloading map image", TextId::MapDownloadFailed},
        {"Failed to commit map cache file", TextId::MapCacheCommitFailed},
    };
    for (const auto &entry : exact) {
        if (error == entry.source) return ui_locale::text(entry.id);
    }
    static constexpr ErrorText prefixes[] = {
        {"AMap connection failed: ", TextId::MapConnectionFailedPrefix},
        {"AMap HTTP error ", TextId::MapHttpErrorPrefix},
    };
    for (const auto &entry : prefixes) {
        const std::string prefix(entry.source);
        if (error.rfind(prefix, 0) == 0) {
            return std::string(ui_locale::text(entry.id)) + error.substr(prefix.size());
        }
    }
    return ui_locale::localize_message(error);
}

class MapActivity final : public Activity {
public:
    ~MapActivity() override
    {
        if (_request) _request->cancel.store(true);
        wifi_setting_release();
    }

    void on_create() override
    {
        amap_config_load(&_config);
        _display_config = _config;
        if (_config.offline || _config.hybrid) {
            if (_config.offline_path.empty()) {
                _message = ui_locale::text(ui_locale::TextId::MapNoPackage);
            } else if (!_offline.open(_config.offline_path.c_str())) {
                _message = offline_error();
            } else {
                if (_config.offline && (!_config.offline_viewport_saved || !_offline.supports(_config.zoom))) {
                    _config.longitude = _offline.center_lon();
                    _config.latitude = _offline.center_lat();
                    _config.zoom = _offline.default_zoom();
                }
                _display_config = _config;
                reload_map();
            }
            if (_config.offline) {
                draw_status(_message, true);
                _dirty = false;
                return;
            }
        }
        // A bare current.img has no viewport identity; do not display it for an
        // unrelated saved center. Verified keyed cache is looked up after EPD idle.
        _has_map = false;
        reload_map();
    }

    bool allow_system_back() const override { return false; }

    void on_show() override
    {
        if (_search_selected) { _search_selected = false; reload_map(); }
        _dirty = _draw_quality = true; // Restore the gray map after the search list.
    }

    void loop() override
    {
        if (M5.Display.displayBusy()) return;
        if (_request && _request->done.load(std::memory_order_acquire)) {
            const bool current = !_exit && !_request->cancel.load() &&
                                 _request->generation == _generation;
            auto result = std::move(_request->result);
            const AmapConfig completed = _request->config;
            _request.reset();
            if (current && result.success) {
                app_paths::ensure_dir(app_paths::map_cache_dir());
                app_paths::ensure_dir(app_paths::map_viewport_cache_dir());
                const std::string key = viewport_key(completed);
                std::string path;
                const bool stored = _cache.store(key, result.path.c_str(), &path);
                if (!stored) {
                    ESP_LOGW(TAG, "viewport cache store failed; retaining single-image fallback");
                    std::remove(app_paths::map_image_file());
                    if (std::rename(result.path.c_str(), app_paths::map_image_file()) == 0)
                        path = app_paths::map_image_file();
                }
                if (!path.empty()) {
                    _image_path = path;
                    _image_key = stored ? key : "";
                    _map_format = result.format;
                    _has_map = true;
                    _gray_frame.reset();
                    _message.clear();
                    amap_config_save_viewport(completed.longitude, completed.latitude, completed.zoom, false,completed.hybrid);
                    _display_config = completed;
                    _draw_quality = true;
                } else _message = localized_map_error("Failed to commit map cache file");
            } else if (current) {
                _message = localized_map_error(result.error);
                _selected_place.clear();
                _config = _display_config;
            }
            if (!current) ESP_LOGI(TAG, "discarded stale/cancelled map response; latest generation=%lu",
                                  static_cast<unsigned long>(_generation));
            if (!result.path.empty()) std::remove(result.path.c_str());
            _dirty = true;
        }
        if (_exit) {
            if (!_request && !M5.Display.displayBusy()) {
                ActivityManager::instance().pop();
            }
            return;
        }
        // No drawing, decode, TLS or Wi-Fi initialization alongside an EPD scan.
        if (M5.Display.displayBusy() || _request) return;
        if (_pending && !_config.offline && !_cache_checked) {
            _cache_checked = true;
            if (!_force_network && load_cached_viewport()) _pending = false;
            else if (_config.api_key.empty()) {
                _pending = false;
                _message = ui_locale::text(ui_locale::TextId::SetAmapKeyHint);
                if (_has_map) _config=_display_config;
                _dirty = true;
            }
        }
        if (_dirty) {
            if (_has_map && !_draw_quality) draw_status_only();
            else draw_current(_draw_quality);
            _dirty = false;
            _draw_quality = false;
            return;
        }
        if (_pending) {
            if (_config.offline) render_offline();
            else start_request();
        }
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && header_layout::left_hit(event.x, event.y, 120))) {
            back();
            return true;
        }
        if (_exit) return true;
        if (event.type == TouchEventType::Tap) {
            if (event.y < HEADER_H && event.x >= 120 && event.x < 260) {
                if (!_request && !_pending && !M5.Display.displayBusy()) {
                    launch_map_route(_config,[this](AmapRoute route) { accept_route(std::move(route)); });
                }
                return true;
            }
            if (header_layout::right_hit(event.x, event.y, M5.Display.width(), 160)) {
                if (!_request && !_pending && !M5.Display.displayBusy()) {
                    launch_map_search(_config, [this](const AmapPlace &place) {
                        _route=AmapRoute{};
                        const auto position = _config.offline
                            ? map_coordinates::gcj02_to_wgs84(place.longitude, place.latitude)
                            : std::make_pair(place.longitude, place.latitude);
                        _config.longitude = position.first; _config.latitude = position.second;
                        _config.zoom = _config.offline ? _offline.default_zoom() : 16;
                        if (_config.offline && _offline.supports(16)) _config.zoom = 16;
                        _selected_place = place.name; _search_selected = true;
                    });
                }
                return true;
            }
            if (event.y<HEADER_H && event.x>=260 && event.x<M5.Display.width()-160) {
                if (!_request && !_pending && !M5.Display.displayBusy()) reload_map(true);
                return true;
            }
            const int x = M5.Display.width() - CONTROL_X_PAD - CONTROL_W;
            if (event.x >= x && event.y >= CONTROL_TOP &&
                event.y < CONTROL_TOP + CONTROL_H) {
                change_zoom(1);
                return true;
            }
            const int minus_y = CONTROL_TOP + CONTROL_H + CONTROL_GAP;
            const int lookup_y = minus_y+CONTROL_H+CONTROL_GAP;
            if (event.x >= M5.Display.width()-CONTROL_X_PAD-156 && event.y >= lookup_y &&
                event.y < lookup_y+2*(CONTROL_H+CONTROL_GAP)) {
                const int relative = event.y-lookup_y;
                if (relative%(CONTROL_H+CONTROL_GAP) >= CONTROL_H) return true;
                if (!_request && !_pending && !M5.Display.displayBusy()) {
                    const bool ip = relative < CONTROL_H+CONTROL_GAP;
                    launch_map_lookup(_display_config,ip,[this](const AmapPlace &place) {
                        _route=AmapRoute{};
                        const auto position = _config.offline
                            ? map_coordinates::gcj02_to_wgs84(place.longitude,place.latitude)
                            : std::make_pair(place.longitude,place.latitude);
                        _config.longitude = position.first; _config.latitude = position.second;
                        _config.zoom = _config.offline ? _offline.default_zoom() : 12;
                        _selected_place = place.name; _search_selected = true;
                    });
                }
                return true;
            }
            if (event.x >= x && event.y >= minus_y && event.y < minus_y + CONTROL_H) {
                change_zoom(-1);
                return true;
            }
            if (!_has_map || !_message.empty()) {
                reload_map(true);
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
            back();
            return true;
        }
        if (_exit) return true;
        if (key == APP_KEY_UP) {
            change_zoom(1);
            return true;
        }
        if (key == APP_KEY_DOWN) {
            change_zoom(-1);
            return true;
        }
        if (key == APP_KEY_OK || key == APP_KEY_EXTRA) {
            reload_map(true);
            return true;
        }
        return false;
    }

private:
    AmapConfig _config;
    AmapConfig _display_config;
    std::shared_ptr<MapRequest> _request;
    uint32_t _generation = 0;
    bool _pending = false, _exit = false, _dirty = false, _draw_quality = false;
    AmapGrayFrame _gray_frame;
    OfflineMapSource _offline;
    AmapImageFormat _map_format = AmapImageFormat::Unknown;
    bool _has_map = false;
    std::string _message;
    MapCache _cache{app_paths::map_viewport_cache_dir()};
    std::string _image_path, _image_key;
    bool _force_network = false, _cache_checked = false;
    bool _search_selected = false;
    std::string _selected_place;
    AmapRoute _route;

    static double mercator_y(double lat) {
        const double r=std::max(-MERCATOR_MAX_LATITUDE,std::min(MERCATOR_MAX_LATITUDE,lat))*PI/180;
        return (1-std::log(std::tan(r)+1/std::cos(r))/PI)/2;
    }

    void accept_route(AmapRoute route) {
        if (!route.success || route.points.empty()) return;
        AmapRoute original=route; // Keep canonical GCJ-02 geometry across source switches.
        if (_config.offline) {
            const auto sw=map_coordinates::gcj02_to_wgs84(route.west,route.south);
            const auto ne=map_coordinates::gcj02_to_wgs84(route.east,route.north);
            route.west=sw.first; route.south=sw.second;
            route.east=ne.first; route.north=ne.second;
            for (auto &point : route.points) point=map_coordinates::gcj02_to_wgs84(point.first,point.second);
            for (auto *point : {&route.origin,&route.destination}) {
                const auto p=map_coordinates::gcj02_to_wgs84(point->longitude,point->latitude);
                point->longitude=p.first; point->latitude=p.second;
            }
        }
        double west=route.origin.longitude,east=west,south=route.origin.latitude,north=south;
        west=std::min(west,route.west); east=std::max(east,route.east);
        south=std::min(south,route.south); north=std::max(north,route.north);
        for (const auto &p : route.points) {
            west=std::min(west,p.first); east=std::max(east,p.first);
            south=std::min(south,p.second); north=std::max(north,p.second);
        }
        west=std::min(west,route.destination.longitude); east=std::max(east,route.destination.longitude);
        south=std::min(south,route.destination.latitude); north=std::max(north,route.destination.latitude);
        const double y0=mercator_y(north),y1=mercator_y(south);
        _config.longitude=(west+east)/2;
        _config.latitude=std::atan(std::sinh(PI*(1-y0-y1)))*180/PI;
        int zoom=map_scale::online_max_zoom;
        for (;zoom>1;--zoom) {
            const double world=map_scale::world_pixels(uint8_t(zoom));
            if ((east-west)/360*world<M5.Display.width()-100 &&
                (y1-y0)*world<M5.Display.height()-HEADER_H-STATUS_H-100) break;
        }
        if (_config.offline) {
            zoom=std::max(int(_offline.min_zoom()),std::min(int(_offline.max_zoom()),zoom));
            while (zoom>_offline.min_zoom() && !_offline.supports(uint8_t(zoom))) --zoom;
        } else zoom=std::max(int(map_scale::online_min_zoom),zoom);
        _config.zoom=uint8_t(zoom); _selected_place.clear(); _route=std::move(original); _search_selected=true;
    }

    void draw_route() {
        if (!_route.success || _route.points.size()<2) return;
        const double world=map_scale::world_pixels(_display_config.zoom);
        auto screen=[&](double lon,double lat) {
            if (_display_config.offline) {
                const auto p=map_coordinates::gcj02_to_wgs84(lon,lat);
                lon=p.first; lat=p.second;
            }
            return std::make_pair(int(std::lround((lon-_display_config.longitude)/360*world+M5.Display.width()/2)),
                int(std::lround((mercator_y(lat)-mercator_y(_display_config.latitude))*world+M5.Display.height()/2)));
        };
        M5.Display.setClipRect(0,HEADER_H,M5.Display.width(),M5.Display.height()-HEADER_H-STATUS_H);
        for (size_t i=1;i<_route.points.size();++i) {
            const auto a=screen(_route.points[i-1].first,_route.points[i-1].second);
            const auto b=screen(_route.points[i].first,_route.points[i].second);
            // White halo separates the route from dark roads on a gray4 map.
            for (int offset=-2;offset<=2;++offset) {
                M5.Display.drawLine(a.first+offset,a.second,b.first+offset,b.second,TFT_WHITE);
                M5.Display.drawLine(a.first,a.second+offset,b.first,b.second+offset,TFT_WHITE);
            }
            M5.Display.drawLine(a.first,a.second,b.first,b.second,TFT_BLACK);
            M5.Display.drawLine(a.first+1,a.second,b.first+1,b.second,TFT_BLACK);
        }
        for (const auto *p : {&_route.origin,&_route.destination}) {
            const auto xy=screen(p->longitude,p->latitude);
            M5.Display.fillCircle(xy.first,xy.second,12,TFT_WHITE);
            M5.Display.drawCircle(xy.first,xy.second,12,TFT_BLACK);
            M5.Display.fillCircle(xy.first,xy.second,p==&_route.origin?4:8,TFT_BLACK);
        }
        M5.Display.clearClipRect();
    }

    std::string viewport_key(const AmapConfig &c) const
    {
        return MapCache::key(c, M5.Display.width(), M5.Display.height(),
                            MYPAPER_AMAP_LOW_RES_PIPELINE ? 1 : 2);
    }

    bool load_cached_viewport()
    {
        const std::string key = viewport_key(_config);
        std::string path;
        const int64_t begin = esp_timer_get_time();
        if (!_cache.lookup(key, &path)) return false;
        const auto format = AmapClient::probe_image(path.c_str());
        if (format == AmapImageFormat::Unknown) { _cache.invalidate(key); return false; }
        _image_path = path; _image_key = key;
        _map_format = format; _gray_frame.reset(); _has_map = true;
        _display_config = _config;
        _message = ui_locale::text(ui_locale::TextId::CachedMap);
        _draw_quality = _dirty = true;
        amap_config_save_viewport(_config.longitude, _config.latitude, _config.zoom,false,_config.hybrid);
        ESP_LOGI(TAG, "viewport cache hit z=%u time=%lld us", unsigned(_config.zoom),
                 static_cast<long long>(esp_timer_get_time()-begin));
        return true;
    }

    void back()
    {
        _exit = true;
        _pending = false;
        if (_request) _request->cancel.store(true);
        ESP_LOGI(TAG, "cancel requested; waiting for network worker cleanup");
        // Keep the Activity alive until its worker releases USB/Wi-Fi/TLS objects.
        // This avoids shutting down a network transport underneath a blocked read.
    }

    void change_zoom(int delta)
    {
        int target = static_cast<int>(_config.zoom) + delta;
        if (_config.hybrid) {
            target=std::max(1,std::min(int(map_scale::online_max_zoom),target));
            while (target<=16 && _offline.is_open() && target>=_offline.min_zoom() &&
                   !_offline.supports(uint8_t(target))) target+=delta;
            if (target<1 || target>map_scale::online_max_zoom || (target<=16 && _offline.is_open() && target<_offline.min_zoom())) return;
        } else if (_config.offline) {
            while (target >= _offline.min_zoom() && target <= _offline.max_zoom() &&
                   !_offline.supports(static_cast<uint8_t>(target))) target += delta;
            if (target < _offline.min_zoom() || target > _offline.max_zoom()) return;
        } else target = std::max(int(map_scale::online_min_zoom), std::min(int(map_scale::online_max_zoom), target));
        if (target == _config.zoom) return;
        _config.zoom = static_cast<uint8_t>(target);
        reload_map();
    }

    void pan(const TouchEvent &event)
    {
        _selected_place.clear();
        int dx = event.dx;
        int dy = event.dy;
        if (dx == 0 && event.type == TouchEventType::SwipeLeft) dx = -M5.Display.width() / 3;
        if (dx == 0 && event.type == TouchEventType::SwipeRight) dx = M5.Display.width() / 3;
        if (dy == 0 && event.type == TouchEventType::SwipeUp) dy = -M5.Display.height() / 3;
        if (dy == 0 && event.type == TouchEventType::SwipeDown) dy = M5.Display.height() / 3;

        const double world_size = map_scale::world_pixels(_config.zoom);
        double world_x = (_config.longitude + 180.0) / 360.0 * world_size;
        const double latitude = std::max(-MERCATOR_MAX_LATITUDE,
                                         std::min(MERCATOR_MAX_LATITUDE, _config.latitude));
        const double radians = latitude * PI / 180.0;
        double world_y = (1.0 - std::log(std::tan(radians) + 1.0 / std::cos(radians)) /
                                  PI) * 0.5 * world_size;
        // Both sources now use the same displayed pixels per geographic unit.
        world_x -= static_cast<double>(dx);
        world_y -= static_cast<double>(dy);
        world_x = std::fmod(world_x, world_size);
        if (world_x < 0.0) world_x += world_size;
        world_y = std::max(0.0, std::min(world_size, world_y));

        const double longitude = world_x / world_size * 360.0 - 180.0;
        const double mercator = PI * (1.0 - 2.0 * world_y / world_size);
        const double new_latitude = std::atan(std::sinh(mercator)) * 180.0 / PI;
        _config.longitude = longitude;
        _config.latitude = new_latitude;
        reload_map();
    }

    bool reload_map(bool force_network = false)
    {
        if (map_source_policy::apply_zoom(_config,_config.zoom))
            ESP_LOGI(TAG,"hybrid source=%s z=%u",_config.offline?"offline":"online",unsigned(_config.zoom));
        if (_config.offline && !_offline.is_open()) {
            _message = ui_locale::text(_config.offline_path.empty() ?
                ui_locale::TextId::MapNoPackage : ui_locale::TextId::MapPackInvalid);
            _dirty = true;
            return false;
        }
        _force_network = force_network;
        _cache_checked = false;
        _message = ui_locale::text(ui_locale::TextId::LoadingMap);
        ++_generation;
        _pending = true;
        _dirty = true;
        if (_request) _request->cancel.store(true);
        return true;
    }

    void start_request()
    {
        _request = std::make_shared<MapRequest>();
        _request->config = _config;
        _request->generation = _generation;
        _request->width = M5.Display.width();
        _request->height = M5.Display.height();
        auto *context = new std::shared_ptr<MapRequest>(_request);
        // NVS/Wi-Fi initialization can disable flash cache: keep the stack internal.
        if (xTaskCreatePinnedToCoreWithCaps(map_fetch_task, "amap_fetch", 16384, context,
                                          1, nullptr, 0,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            delete context;
            _request.reset();
            _message = localized_map_error("AMap connection failed: ESP_ERR_NO_MEM");
            if (_has_map) _config=_display_config;
            _dirty = true;
        }
        _pending = false;
        ESP_LOGI(TAG, "request queued generation=%lu", static_cast<unsigned long>(_generation));
    }

    std::string offline_error() const
    {
        using ui_locale::TextId;
        TextId id = TextId::MapPackInvalid;
        switch (_offline.error()) {
        case OfflineMap::Error::Io: id = TextId::MapReadFailed; break;
        case OfflineMap::Error::Memory: id = TextId::MapOutOfMemory; break;
        case OfflineMap::Error::Range: id = TextId::MapOutOfRange; break;
        case OfflineMap::Error::Missing: id = TextId::MapMissingTiles; break;
        default: break;
        }
        return ui_locale::text(id);
    }

    void render_offline()
    {
        _pending = false;
        const int64_t begin = esp_timer_get_time();
        if (_offline.render(_config.longitude, _config.latitude, _config.zoom,
                            M5.Display.width(), M5.Display.height())) {
            _has_map = true;
            _display_config = _config;
            _message.clear();
            amap_config_save_viewport(_config.longitude, _config.latitude, _config.zoom, true,_config.hybrid);
            _draw_quality = true;
        } else {
            _message = offline_error();
            _selected_place.clear();
            _config = _display_config;
        }
        const auto stats = _offline.stats();
        ESP_LOGI(TAG, "offline viewport z=%u tiles_read=%lu cache_hits=%lu time=%lld us pack=%s",
                 unsigned(_config.zoom), static_cast<unsigned long>(stats.read),
                 static_cast<unsigned long>(stats.hits),
                 static_cast<long long>(esp_timer_get_time()-begin), _offline.active_path());
        _dirty = true;
    }

    bool draw_image()
    {
        if (_display_config.offline) {
            const auto *data = _offline.frame();
            if (!data) return false;
            auto *panel = M5.Display.getPanel();
            if (panel && panel->writeNativeGrayscale4(data, M5.Display.width(), M5.Display.height())) return true;
            // Generic backend fallback preserves all 16 gray levels.
            const int width = M5.Display.width();
            for (int y = 0; y < M5.Display.height(); ++y) {
                for (int x = 0; x < width; ++x) {
                    const size_t n = size_t(y)*width+x;
                    const uint8_t g = ((data[n/2] >> ((n&1) ? 0 : 4)) & 15)*17;
                    M5.Display.drawPixel(x, y, M5.Display.color565(g,g,g));
                }
                if (y % 64 == 63) vTaskDelay(1);
            }
            return true;
        }
        const char *path = _image_path.c_str();
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
        system_font::draw_left(M5.Display, "<", 18, header_layout::text_y(), TFT_BLACK);
        system_font::draw_left(M5.Display,ui_locale::text(ui_locale::TextId::MapRoute),125,header_layout::text_y(),TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::Map), width / 2, header_layout::text_y(), TFT_BLACK);
        system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::MapSearchAction), width-18, header_layout::text_y(), TFT_BLACK);
        header_layout::draw_separator(M5.Display);
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
        const int lookup_x = M5.Display.width()-CONTROL_X_PAD-156;
        for (int i = 0; i < 2; ++i) {
            const int y = minus_y+(i+1)*(CONTROL_H+CONTROL_GAP);
            M5.Display.fillRoundRect(lookup_x,y,156,CONTROL_H,8,TFT_WHITE);
            M5.Display.drawRoundRect(lookup_x,y,156,CONTROL_H,8,TFT_BLACK);
            system_font::draw_center(M5.Display,system_font::ellipsize(ui_locale::text(i == 0 ?
                ui_locale::TextId::MapIpLocation : ui_locale::TextId::MapAddress),140),lookup_x+78,y+13,TFT_BLACK);
        }
    }

    bool draw_status_bar(const std::string &message)
    {
        // Hide routine prompts and coordinates. Only errors overlay the map.
        if (message.empty() || message==ui_locale::text(ui_locale::TextId::LoadingMap) ||
            message==ui_locale::text(ui_locale::TextId::CachedMap)) return false;
        const int width = M5.Display.width();
        const int y = HEADER_H;
        M5.Display.fillRect(0, y, width, 70, TFT_WHITE);
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(message, width - 30),
                                 width / 2, y + 11, TFT_BLACK);
        return true;
    }

    void draw_status_only()
    {
        if (_message.empty() || _message==ui_locale::text(ui_locale::TextId::LoadingMap) ||
            _message==ui_locale::text(ui_locale::TextId::CachedMap)) return;
        // Keep the existing gray map untouched while showing loading/error text.
        // Importing the whole map for a Text status update would quantize it again.
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_status_bar(_message);
        M5.Display.display(0, HEADER_H, M5.Display.width(), 70);
        M5.Display.endWrite();
    }

    void draw_map(bool quality, const std::string &message)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        if (!draw_image()) {
            if (!_display_config.offline && !_image_key.empty()) _cache.invalidate(_image_key);
            _has_map = false;
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::MapDecodeFailed),
                                     M5.Display.width() / 2,
                                     M5.Display.height() / 2, TFT_BLACK);
        }
        draw_route();
        draw_header();
        draw_controls();
        if (!_selected_place.empty() && _config.longitude == _display_config.longitude &&
            _config.latitude == _display_config.latitude) {
            const int cx = M5.Display.width()/2, cy = M5.Display.height()/2;
            M5.Display.fillCircle(cx, cy, 12, TFT_WHITE);
            M5.Display.drawCircle(cx, cy, 12, TFT_BLACK);
            M5.Display.fillCircle(cx, cy, 5, TFT_BLACK);
            const auto name = system_font::ellipsize(_selected_place, M5.Display.width()-40);
            const int label_width = system_font::measure(name)+16;
            M5.Display.fillRect(cx-label_width/2, cy+18, label_width, system_font::line_height(), TFT_WHITE);
            system_font::draw_center(M5.Display, name, cx, cy+18, TFT_BLACK);
        }
        if (_display_config.offline) {
            const int y = M5.Display.height()-STATUS_H-system_font::line_height()-4;
            M5.Display.fillRect(0, y, M5.Display.width(), system_font::line_height()+4, TFT_WHITE);
            system_font::draw_left(M5.Display, "© OpenStreetMap contributors", 12, y, TFT_BLACK);
        }
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
        draw_controls();
        system_font::draw_center(M5.Display, system_font::ellipsize(message,
                                 M5.Display.width() - 40), M5.Display.width() / 2,
                                 M5.Display.height() / 2 - 20, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_current(bool quality)
    {
        if (_has_map) draw_map(quality, _message);
        else draw_status(_message.empty() ? ui_locale::text(ui_locale::TextId::TapToLoadMap)
                                          : _message, quality);
    }
};

} // namespace

void launch_map()
{
    AmapConfig config; amap_config_load(&config);
    if (!config.offline && !config.hybrid && !ensure_wifi_configured_or_show_guide()) return;
    ActivityManager::instance().push(new MapActivity());
}
