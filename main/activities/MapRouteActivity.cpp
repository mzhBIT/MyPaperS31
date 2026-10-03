#include "activities/MapRouteActivity.h"
#include "activities/MapSearchActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/HeaderLayout.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
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
constexpr int ROW=80, FOOT=76;
struct Request {
    AmapConfig config; AmapPlace start,end; AmapTravelMode mode;
    AmapRoute result; std::atomic<bool> done{false},cancel{false};
};
void fetch(void *arg)
{
    std::unique_ptr<std::shared_ptr<Request>> holder(static_cast<std::shared_ptr<Request> *>(arg));
    auto request=*holder;
    const auto err=request->cancel.load() ? ESP_ERR_INVALID_STATE : wifi_setting_connect_saved(15000);
    if (err==ESP_OK && !request->cancel.load()) {
        AmapClient client;
        request->result=client.plan_route(request->config,request->start,request->end,request->mode,&request->cancel);
        client.close();
    } else request->result.error=esp_err_to_name(err);
    request->done.store(true,std::memory_order_release);
    request.reset(); holder.reset(); vTaskDeleteWithCaps(nullptr);
}
class MapRouteActivity final : public Activity {
    AmapConfig _config; AmapPlace _start,_end;
    AmapTravelMode _mode=AmapTravelMode::Driving;
    AmapRoute _route;
    std::function<void(AmapRoute)> _show;
    std::shared_ptr<Request> _request;
    std::vector<std::string> _lines;
    std::string _status;
    bool _dirty=true,_quality=true,_exit=false,_pending=false;
    int _page=0;
    int body_y() const { return header_layout::height+4*ROW; }
    int lines_per_page() const { return std::max(1,int((M5.Display.height()-FOOT-body_y()-16)/(system_font::line_height()+8))); }
    void reset_result() { _route=AmapRoute{}; _lines.clear(); _page=0; _status.clear(); _dirty=true; }
    void back() { _exit=true; _pending=false; if (_request) _request->cancel.store(true); }
    void choose(bool start) {
        if (_request || _pending) return;
        launch_map_search(_config,[this,start](const AmapPlace &place) {
            if (start) _start=place; else _end=place;
            reset_result();
        });
    }
    void plan() {
        if (_request || _pending) return;
        if (_start.name.empty() || _end.name.empty()) { _status=tr(TextId::MapRouteChooseBoth); _dirty=true; return; }
        reset_result(); _pending=true; _status=tr(TextId::MapRouteLoading);
    }
    void prepare_lines() {
        _lines.clear();
        for (size_t i=0;i<_route.steps.size();++i) {
            const std::string text=std::to_string(i+1)+". "+_route.steps[i];
            std::string line;
            for (size_t p=0;p<text.size();) {
                const unsigned char c=text[p]; size_t n=c<128?1:c<224?2:c<240?3:4;
                auto glyph=text.substr(p,std::min(n,text.size()-p));
                if (!line.empty() && system_font::measure(line+glyph)>M5.Display.width()-36) { _lines.push_back(line); line.clear(); }
                line+=glyph; p+=glyph.size();
            }
            if (!line.empty()) _lines.push_back(line);
            _lines.push_back("");
        }
    }
    void draw() {
        auto &d=M5.Display; const int w=d.width(),h=d.height(),top=header_layout::height;
        d.startWrite(); d.setEpdMode(_quality?epd_mode_t::epd_quality:epd_mode_t::epd_text); d.fillScreen(TFT_WHITE);
        system_font::draw_left(d,"<",18,header_layout::text_y());
        system_font::draw_center(d,tr(TextId::MapRoute),w/2,header_layout::text_y());
        system_font::draw_right(d,tr(TextId::MapRouteSwap),w-18,header_layout::text_y());
        header_layout::draw_separator(d);
        system_font::draw_left(d,system_font::ellipsize(std::string(tr(TextId::MapRouteStart))+": "+
            (_start.name.empty()?tr(TextId::MapRouteEnter):_start.name),w-36),18,top+14);
        d.drawFastHLine(18,top+ROW-1,w-36,TFT_BLACK);
        system_font::draw_left(d,system_font::ellipsize(std::string(tr(TextId::MapRouteEnd))+": "+
            (_end.name.empty()?tr(TextId::MapRouteEnter):_end.name),w-36),18,top+ROW+14);
        d.drawFastHLine(18,top+2*ROW-1,w-36,TFT_BLACK);
        const TextId modes[]={TextId::MapRouteDriving,TextId::MapRouteWalking,TextId::MapRouteCycling};
        for (int i=0;i<3;++i) {
            const int x=i*w/3,ww=(i+1)*w/3-x;
            if (int(_mode)==i) d.fillRect(x,top+2*ROW,ww,ROW,TFT_BLACK);
            system_font::draw_center(d,tr(modes[i]),x+ww/2,top+2*ROW+14,int(_mode)==i?TFT_WHITE:TFT_BLACK);
        }
        std::string summary=_status;
        if (_route.success) {
            char distance[64]; std::snprintf(distance,sizeof(distance),"%.2f km",_route.distance/1000);
            summary=distance;
            if (_route.duration>=0) summary+="  "+std::to_string((_route.duration+59)/60)+" "+tr(TextId::MapRouteMinutes);
        }
        system_font::draw_left(d,system_font::ellipsize(summary,w-36),18,top+3*ROW+14);
        d.drawFastHLine(18,body_y()-1,w-36,TFT_BLACK);
        int y=body_y()+8;
        for (int r=0;r<lines_per_page();++r) {
            const size_t i=size_t(_page*lines_per_page()+r); if (i>=_lines.size()) break;
            if (_lines[i].empty()) d.drawFastHLine(18,y+system_font::line_height()/2,w-36,TFT_LIGHTGREY);
            else system_font::draw_left(d,_lines[i],18,y);
            y+=system_font::line_height()+8;
        }
        d.drawFastHLine(0,h-FOOT,w,TFT_BLACK);
        system_font::draw_left(d,tr(TextId::MapRoutePlan),18,h-FOOT+14);
        system_font::draw_right(d,tr(TextId::MapRouteShow),w-18,h-FOOT+14,_route.success?TFT_BLACK:TFT_DARKGREY);
        system_font::draw_center(d,std::to_string(_page+1)+"/"+std::to_string(std::max<size_t>(1,(_lines.size()+lines_per_page()-1)/lines_per_page())),w/2,h-FOOT+14);
        d.display(); d.endWrite(); _dirty=false; _quality=false;
    }
public:
    MapRouteActivity(const AmapConfig &config,std::function<void(AmapRoute)> show):_config(config),_show(std::move(show)) {}
    ~MapRouteActivity() override { wifi_setting_release(); }
    void on_create() override { _status=tr(TextId::MapRouteChooseBoth); }
    void on_show() override { _dirty=_quality=true; }
    bool allow_system_back() const override { return false; }
    void loop() override {
        if (M5.Display.displayBusy()) return;
        if (_request && _request->done.load(std::memory_order_acquire)) {
            if (!_exit) {
                _route=std::move(_request->result);
                _status=_route.success?"":ui_locale::localize_message(_route.error);
                if (_route.success) prepare_lines();
                _dirty=true;
            }
            _request.reset();
        }
        if (_exit && !_request) { ActivityManager::instance().pop(); return; }
        if (_exit || _request) return;
        if (_dirty) { draw(); return; }
        if (_pending) {
            _pending=false; _request=std::make_shared<Request>();
            _request->config=_config; _request->start=_start; _request->end=_end; _request->mode=_mode;
            auto *context=new std::shared_ptr<Request>(_request);
            if (xTaskCreatePinnedToCoreWithCaps(fetch,"amap_route",16384,context,1,nullptr,0,
                MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
                delete context; _request.reset(); _status=tr(TextId::MapOutOfMemory); _dirty=true;
            }
        }
    }
    bool on_key(int key) override {
        if (key==APP_KEY_BACK) { back(); return true; }
        if (key==APP_KEY_OK) { plan(); return true; }
        return false;
    }
    bool on_touch(const TouchEvent &e) override {
        if (e.type==TouchEventType::Back || (e.type==TouchEventType::Tap && header_layout::left_hit(e.x,e.y,100))) { back(); return true; }
        if (_exit || _request || _pending || M5.Display.displayBusy()) return true;
        if (e.type==TouchEventType::SwipeUp || e.type==TouchEventType::SwipeDown) {
            _page=std::max(0,std::min(_page+(e.type==TouchEventType::SwipeUp?1:-1),int((_lines.empty()?0:_lines.size()-1)/lines_per_page()))); _dirty=true; return true;
        }
        if (e.type!=TouchEventType::Tap) return false;
        if (header_layout::right_hit(e.x,e.y,M5.Display.width(),130)) { std::swap(_start,_end); reset_result(); }
        else if (e.y<header_layout::height) return true;
        else if (e.y<header_layout::height+ROW) choose(true);
        else if (e.y<header_layout::height+2*ROW) choose(false);
        else if (e.y<header_layout::height+3*ROW) { _mode=AmapTravelMode(std::max(0,std::min(2,int(e.x)*3/int(M5.Display.width())))); reset_result(); }
        else if (e.y>=M5.Display.height()-FOOT) {
            if (e.x<M5.Display.width()/3) plan();
            else if (e.x>=M5.Display.width()*2/3 && _route.success && !_route.points.empty()) { _show(std::move(_route)); back(); }
        }
        return true;
    }
};
}
void launch_map_route(const AmapConfig &config,std::function<void(AmapRoute)> show)
{
    ActivityManager::instance().push(new MapRouteActivity(config,std::move(show)));
}
