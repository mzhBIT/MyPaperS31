#include "activities/OfflineMapPickerActivity.h"
#include "activities/ActivityManager.h"
#include "map/AmapConfig.h"
#include "map/OfflineMapSource.h"
#include "ui/HeaderLayout.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include <M5Unified.h>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <vector>
#include <string>
#include <esp_log.h>
#include <cerrno>

namespace {
using ui_locale::TextId;
const char *tr(TextId id) { return ui_locale::text(id); }
constexpr int ROW = 84, FOOT = 72;
struct Entry { std::string name; bool dir; };
class Picker final : public Activity {
    std::string _path = "/sdcard", _message;
    std::vector<Entry> _entries;
    int _page = 0, _focus = 0;
    bool _dirty = true;
    int rows() const { return std::max(1, (int(M5.Display.height())-header_layout::height-FOOT)/ROW); }
    void load(const std::string &path) {
        _path = path; _entries.clear(); _page = _focus = 0; _message.clear();
        DIR *d = opendir(path.c_str());
        if (!d) {
            ESP_LOGW("MAP_PICKER", "directory open failed path=%s errno=%d", path.c_str(), errno);
            _message = tr(TextId::MapReadFailed); _dirty = true; return;
        }
        while (auto *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string name = e->d_name, full = path + "/" + name;
            struct stat st{};
            if (stat(full.c_str(), &st)) continue;
            bool directory = S_ISDIR(st.st_mode);
            if (!directory && (name.size()<4 || (strcasecmp(name.c_str()+name.size()-4, ".mpm") &&
                                                strcasecmp(name.c_str()+name.size()-4, ".mpc")))) continue;
            _entries.push_back({name, directory});
            if (_entries.size() >= 512) break;
        }
        closedir(d);
        ESP_LOGI("MAP_PICKER", "directory=%s visible_entries=%u filter=directories/mpm/mpc",
                 path.c_str(), unsigned(_entries.size()));
        std::sort(_entries.begin(), _entries.end(), [](const Entry &a, const Entry &b) {
            if (a.dir != b.dir) return a.dir > b.dir;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        _dirty = true;
    }
    void back() {
        if (_path == "/sdcard") { ActivityManager::instance().pop(); return; }
        load(_path.substr(0, _path.find_last_of('/')));
    }
    void activate(int index) {
        if (index < 0 || index >= int(_entries.size())) return;
        const auto e = _entries[index];
        std::string full = _path + "/" + e.name;
        if (e.dir) { load(full); return; }
        OfflineMapSource map;
        ESP_LOGI("MAP_PICKER", "selected path=%s", full.c_str());
        if (!map.open(full.c_str())) {
            ESP_LOGW("MAP_PICKER", "selection failed path=%s error=%d", full.c_str(), int(map.error()));
            _message = tr(map.error() == OfflineMap::Error::Memory ? TextId::MapOutOfMemory :
                          map.error() == OfflineMap::Error::Io ? TextId::MapReadFailed : TextId::MapPackInvalid);
            _dirty = true; return;
        }
        map.close(); // Free index before NVS/next Activity.
        if (!amap_config_save_offline_path(full)) {
            _message = tr(TextId::MapSaveFailed); _dirty = true; return;
        }
        ActivityManager::instance().pop();
    }
    void draw() {
        auto &d = M5.Display; const int w = d.width(), h = d.height();
        d.startWrite(); d.setEpdMode(epd_mode_t::epd_text); d.fillScreen(TFT_WHITE);
        system_font::draw_left(d, "<", 18, header_layout::text_y());
        system_font::draw_center(d, tr(TextId::SelectMapPackage), w/2, header_layout::text_y());
        header_layout::draw_separator(d);
        for (int r=0; r<rows(); ++r) {
            int i = _page*rows()+r;
            if (i >= int(_entries.size())) break;
            int y = header_layout::height+r*ROW;
            if (i == _focus) d.fillRect(0, y, w, ROW-1, TFT_LIGHTGREY);
            system_font::draw_left(d, system_font::ellipsize((_entries[i].dir ? "[+] " : "")+_entries[i].name, w-36), 18, y+14);
            d.drawFastHLine(18, y+ROW-1, w-36, TFT_BLACK);
        }
        system_font::draw_left(d, system_font::ellipsize(_message.empty() ? _path : _message, w-36), 18, h-FOOT+12);
        d.display(); d.endWrite(); _dirty = false;
    }
public:
    void on_create() override {
        AmapConfig c; amap_config_load(&c);
        if (c.offline_path.rfind("/sdcard/", 0) == 0) load(c.offline_path.substr(0,c.offline_path.find_last_of('/')));
        else load("/sdcard");
    }
    bool allow_system_back() const override { return false; }
    void loop() override { if (_dirty && !M5.Display.displayBusy()) draw(); }
    bool on_touch(const TouchEvent &e) override {
        if (M5.Display.displayBusy()) return true;
        if (e.type == TouchEventType::Back || (e.type == TouchEventType::Tap && header_layout::left_hit(e.x,e.y,120))) { back(); return true; }
        if (e.type == TouchEventType::SwipeUp || e.type == TouchEventType::SwipeDown) {
            _page = std::max(0,std::min(_page+(e.type == TouchEventType::SwipeUp ? 1 : -1), int((_entries.empty()?0:_entries.size()-1)/rows())));
            _dirty = true; return true;
        }
        if (e.type == TouchEventType::Tap && e.y >= header_layout::height && e.y < header_layout::height+rows()*ROW)
            activate(_page*rows()+(e.y-header_layout::height)/ROW);
        return true;
    }
    bool on_key(int key) override {
        if (M5.Display.displayBusy()) return true;
        if (key == APP_KEY_BACK) { back(); return true; }
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            _focus = std::max(0,std::min(_focus+(key == APP_KEY_UP ? -1 : 1), int(_entries.size())-1));
            _page = _focus/rows(); _dirty = true;
        } else if (key == APP_KEY_OK) activate(_focus);
        return true;
    }
};
}
void launch_offline_map_picker() { ActivityManager::instance().push(new Picker()); }
