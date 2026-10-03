#include "activities/MapSearchActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/TextInputOverlay.h"
#include "ui/HeaderLayout.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "map/MapCoordinates.h"
#include "wifi/wifi_setting.h"
#include <M5Unified.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <memory>
#include <algorithm>
#include <cstdio>

namespace {
using ui_locale::TextId;
const char *tr(TextId id) { return ui_locale::text(id); }
constexpr int ROWS = 5, FOOTER = 70;
struct SearchRequest {
    AmapConfig config;
    std::string keyword;
    int kind = 0; // 0: POI, 1: IP, 2: reverse geocode.
    AmapSearchResult result;
    std::atomic<bool> done{false}, cancel{false};
};
void fetch(void *arg)
{
    std::unique_ptr<std::shared_ptr<SearchRequest>> holder(static_cast<std::shared_ptr<SearchRequest> *>(arg));
    auto request = *holder;
    const auto err = request->cancel.load() ? ESP_ERR_INVALID_STATE : wifi_setting_connect_saved(15000);
    if (err == ESP_OK && !request->cancel.load()) {
        AmapClient client;
        request->result = request->kind == 0
            ? client.search_places(request->config, request->keyword, &request->cancel)
            : client.lookup_location(request->config, request->kind == 1, &request->cancel);
        client.close();
    } else request->result.error = esp_err_to_name(err);
    request->done.store(true, std::memory_order_release);
    request.reset(); holder.reset(); vTaskDeleteWithCaps(nullptr);
}
class MapSearchActivity final : public Activity {
    AmapConfig _config;
    std::function<void(const AmapPlace &)> _selected;
    ui::TextInputOverlay _input;
    std::shared_ptr<SearchRequest> _request;
    std::vector<AmapPlace> _places;
    std::string _pending, _status;
    bool _dirty = true, _exit = false, _first_draw = true;
    int _page = 0;
    int _kind = 0;
    int row_height() const { return (M5.Display.height()-header_layout::height-FOOTER)/ROWS; }
    void back() { _exit = true; _pending.clear(); if (_request) _request->cancel.store(true); }
    void input() {
        if (_request || _exit || M5.Display.displayBusy()) return;
        if (_kind) { _pending = "lookup"; _places.clear(); _status = tr(TextId::MapLocating); _dirty = true; return; }
        TextInputOptions options; options.title = tr(TextId::MapSearchHint);
        options.max_bytes = 240; options.start_in_chinese = true;
        _input.open(options, [this](const std::string &value) {
            _pending = value; _places.clear(); _page = 0; _status = tr(TextId::MapSearching); _dirty = true;
        }, [this] { _dirty = true; });
    }
    void draw() {
        auto &d = M5.Display; const int w = d.width(), h = d.height();
        d.startWrite(); d.setEpdMode(_first_draw ? epd_mode_t::epd_quality : epd_mode_t::epd_text); d.fillScreen(TFT_WHITE);
        system_font::draw_left(d, "<", 18, header_layout::text_y());
        system_font::draw_center(d, tr(_kind == 1 ? TextId::MapIpLocation : _kind == 2 ? TextId::MapAddress : TextId::MapSearch), w/2, header_layout::text_y());
        if (!_kind) system_font::draw_right(d, tr(TextId::MapSearchAction), w-18, header_layout::text_y());
        header_layout::draw_separator(d);
        for (int r = 0; !_kind && r < ROWS; ++r) {
            const size_t i = size_t(_page*ROWS+r); if (i >= _places.size()) break;
            const int y = header_layout::height+r*row_height();
            system_font::draw_left(d, system_font::ellipsize(_places[i].name, w-36), 18, y+10);
            if (row_height() >= 2*system_font::line_height()+22)
                system_font::draw_left(d, system_font::ellipsize(_places[i].address, w-36),
                                      18, y+system_font::line_height()+18, TFT_DARKGREY);
            d.drawFastHLine(18, y+row_height()-1, w-36, TFT_BLACK);
        }
        if (_kind && !_places.empty()) {
            const auto &place = _places.front();
            int y = header_layout::height+16;
            system_font::draw_left(d, system_font::ellipsize(place.name,w-36),18,y);
            y += system_font::line_height()+12;
            char position[80]; std::snprintf(position,sizeof(position),"%.6f, %.6f (GCJ-02)",place.longitude,place.latitude);
            system_font::draw_left(d, system_font::ellipsize(position,w-36),18,y);
            y += system_font::line_height()+16;
            // Wrap UTF-8 on glyph boundaries, so long addresses are not hidden.
            const std::string text = _kind == 1 ? tr(TextId::MapIpHint) : place.address;
            std::string line;
            for (size_t p = 0; p < text.size();) {
                const unsigned char c = text[p]; const size_t count = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
                const auto glyph = text.substr(p,std::min(count,text.size()-p));
                if (!line.empty() && system_font::measure(line+glyph)>w-36) {
                    if (y+system_font::line_height() > h-FOOTER) break;
                    system_font::draw_left(d,line,18,y); y += system_font::line_height()+8; line.clear();
                }
                line += glyph; p += glyph.size();
            }
            if (!line.empty() && y+system_font::line_height() <= h-FOOTER) system_font::draw_left(d,line,18,y);
        }
        if (_places.empty()) system_font::draw_center(d, system_font::ellipsize(_status, w-36), w/2, header_layout::height+30);
        d.drawFastHLine(0, h-FOOTER, w, TFT_BLACK);
        system_font::draw_left(d, tr(_kind ? TextId::RefreshAction : TextId::MapSearchAction), 18, h-FOOTER+10);
        if (_kind) system_font::draw_right(d, tr(_kind == 1 && !_places.empty() ? TextId::MapUseLocation : TextId::Cancel), w-18,h-FOOTER+10);
        else system_font::draw_right(d, std::to_string(_page+1)+"/"+
            std::to_string(std::max<size_t>(1, (_places.size()+ROWS-1)/ROWS)), w-18, h-FOOTER+10);
        d.display(); d.endWrite(); _dirty = false; _first_draw = false;
    }
public:
    MapSearchActivity(const AmapConfig &config, std::function<void(const AmapPlace &)> selected, int kind = 0)
        : _config(config), _selected(std::move(selected)), _kind(kind) {}
    ~MapSearchActivity() override { wifi_setting_release(); }
    void on_create() override {
        _status = tr(_kind ? TextId::MapLocating : TextId::MapSearchHint);
        if (_kind) _pending = "lookup";
    }
    bool allow_system_back() const override { return false; }
    void loop() override {
        if (M5.Display.displayBusy() || _input.visible()) return;
        if (_request && _request->done.load(std::memory_order_acquire)) {
            if (!_exit) {
                if (_request->result.success) {
                    _places = std::move(_request->result.places);
                    _status = _places.empty() ? tr(TextId::MapSearchEmpty) : "";
                } else _status = ui_locale::localize_message(_request->result.error);
                _dirty = true;
            }
            _request.reset();
        }
        if (_exit && !_request) { ActivityManager::instance().pop(); return; }
        if (_exit || _request) return;
        if (_dirty) { draw(); return; }
        if (!_pending.empty()) {
            _request = std::make_shared<SearchRequest>(); _request->config = _config;
            _request->kind = _kind;
            if (_kind == 2 && _config.offline) {
                const auto point = map_coordinates::wgs84_to_gcj02(_config.longitude,_config.latitude);
                _request->config.longitude = point.first; _request->config.latitude = point.second;
            }
            _request->keyword = std::move(_pending); _pending.clear();
            auto *context = new std::shared_ptr<SearchRequest>(_request);
            if (xTaskCreatePinnedToCoreWithCaps(fetch, "amap_search", 16384, context, 1, nullptr, 0,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
                delete context; _request.reset(); _status = tr(TextId::MapOutOfMemory); _dirty = true;
            }
        }
    }
    bool on_key(int key) override {
        if (_input.visible()) return _input.handle_key(key);
        if (key == APP_KEY_BACK) { back(); return true; }
        if (key == APP_KEY_OK) { input(); return true; }
        return false;
    }
    bool on_touch(const TouchEvent &e) override {
        if (_input.visible()) return _input.handle_touch(e);
        if (e.type == TouchEventType::Back || (e.type == TouchEventType::Tap && header_layout::left_hit(e.x,e.y,100))) { back(); return true; }
        if (_exit || _request || M5.Display.displayBusy()) return true;
        if (_kind) {
            if (e.type == TouchEventType::Tap && e.y >= M5.Display.height()-FOOTER) {
                if (e.x < M5.Display.width()/2) input();
                else {
                    if (_kind == 1 && !_places.empty()) _selected(_places.front());
                    back();
                }
            }
            return true;
        }
        if (e.type == TouchEventType::Tap && (header_layout::right_hit(e.x,e.y,M5.Display.width(),160) || e.y >= M5.Display.height()-FOOTER)) { input(); return true; }
        if (e.type == TouchEventType::SwipeUp || e.type == TouchEventType::SwipeDown) {
            _page = std::max(0, std::min(_page+(e.type == TouchEventType::SwipeUp ? 1 : -1), int((_places.empty()?0:_places.size()-1)/ROWS)));
            _dirty = true; return true;
        }
        if (e.type == TouchEventType::Tap && e.y >= header_layout::height && e.y < M5.Display.height()-FOOTER) {
            const size_t i = _page*ROWS+(e.y-header_layout::height)/row_height();
            if (i < _places.size()) { _selected(_places[i]); back(); }
            return true;
        }
        return false;
    }
};
}
void launch_map_search(const AmapConfig &config, std::function<void(const AmapPlace &)> selected)
{
    ActivityManager::instance().push(new MapSearchActivity(config, std::move(selected)));
}
void launch_map_lookup(const AmapConfig &config, bool ip, std::function<void(const AmapPlace &)> selected)
{
    ActivityManager::instance().push(new MapSearchActivity(config,std::move(selected),ip ? 1 : 2));
}
