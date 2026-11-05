"""Generate PC regression from the real driver and preference reset functions."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else '.codex-build-cache/settings-reset')
out.mkdir(parents=True, exist_ok=True)
wifi = (root / 'main/wifi/wifi_setting.c').read_text(encoding='utf-8')
start = wifi.index('esp_err_t wifi_setting_reset_driver_config(void)')
driver = wifi[start:]
settings = (root / 'main/storage/settings_reset.cpp').read_text(encoding='utf-8')
settings = '\n'.join(line for line in settings.splitlines() if not line.startswith('#include'))
settings = settings.replace('std::remove(path)', 'fake_remove(path)')
(out / 'test.cpp').write_text(r'''
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using esp_err_t=int;
using nvs_handle_t=int;
using wifi_setting_power_mode_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NVS_NOT_FOUND=-2, IO_ERROR=-3;
constexpr int NVS_READONLY=0, NVS_READWRITE=1;
constexpr int WIFI_STORAGE_FLASH=0, WIFI_STORAGE_RAM=1;
constexpr int WIFI_SETTING_POWER_ON_DEMAND=0, WIFI_SETTING_POWER_ALWAYS_ON=1;
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
static int s_power_mode=1;
static bool s_wifi_initialized=false;
std::map<std::string,std::map<std::string,std::string>> nvs;
std::map<int,std::string> handles;
std::vector<std::string> operations;
std::string failure, ignore_erase;
unsigned deinit_calls=0;
bool sd_failure=false, driver_live=false;
int step(const std::string& name) {
    operations.push_back(name);
    return failure==name ? IO_ERROR : ESP_OK;
}
int wifi_setting_deinit(bool power_down) {
    assert(power_down); ++deinit_calls;
    int result=step(deinit_calls==1?"stop-before":"stop-after");
    if(result==ESP_OK) s_wifi_initialized=driver_live=false;
    return result;
}
int nvs_flash_init() { return step("nvs-init"); }
int wifi_setting_network_stack_init() { return step("net-init"); }
int wifi_setting_c6_power_on() { return step("rail-on"); }
int esp_wifi_init(wifi_init_config_t*) {
    assert(!driver_live); int result=step("driver-init");
    if(!result)driver_live=true;
    return result;
}
int esp_wifi_set_storage(int mode) {
    assert(driver_live);
    return step(mode==WIFI_STORAGE_FLASH?"storage-flash":"storage-ram");
}
int esp_wifi_restore() { assert(driver_live); return step("sdk-restore"); }
namespace app_paths {
const char* reader_style_file() { return "style"; }
const char* reader_font_file() { return "font"; }
}
int fake_remove(const char*) {
    errno=sd_failure?EIO:ENOENT; return -1;
}
int nvs_open(const char* name,int mode,nvs_handle_t* handle) {
    if(mode==NVS_READONLY && !nvs.count(name))return ESP_ERR_NVS_NOT_FOUND;
    *handle=int(handles.size())+1; handles[*handle]=name; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { handles.erase(handle); }
int nvs_erase_all(nvs_handle_t handle) {
    auto name=handles.at(handle);
    int result=step("erase:"+name);
    if(!result && ignore_erase!=name)nvs[name].clear();
    return result;
}
int nvs_erase_key(nvs_handle_t handle,const char* key) {
    return nvs[handles.at(handle)].erase(key)?ESP_OK:ESP_ERR_NVS_NOT_FOUND;
}
int nvs_commit(nvs_handle_t handle) { return step("commit:"+handles.at(handle)); }
int nvs_get_str(nvs_handle_t handle,const char* key,char*,size_t* size) {
    const auto name=handles.at(handle);
    operations.push_back("verify:"+name+"/"+key);
    if(failure=="verify:"+name)return IO_ERROR;
    const auto& values=nvs[name];
    auto it=values.find(key);
    if(it==values.end())return ESP_ERR_NVS_NOT_FOUND;
    *size=it->second.size()+1; return ESP_OK;
}
''' + driver + '\n' + settings + r'''
void fixture() {
    nvs.clear();handles.clear();operations.clear();failure.clear();ignore_erase.clear();
    deinit_calls=0;sd_failure=false;driver_live=s_wifi_initialized=false;s_power_mode=1;
    nvs["wifi_cfg"]={{"ssid","old-router"},{"password","test-password"},{"power_mode","1"}};
    nvs["ds_chat"]={{"api_key","test-deepseek-key"},{"model","1"}};
    nvs["amap"]={{"api_key","test-map-key"},{"last_zoom","16"}};
    nvs["weread"]={{"cookies","login"},{"api_key","session-token"}};
    nvs["stocks"]={{"watchlist","my-list"}};
    nvs["calibration"]={{"vcom","1230"}};
    nvs["phone"]={{"ringtone","my-tone"},{"messages","history"}};
}
int main() {
    fixture(); assert(settings_restore_defaults()==ESP_OK);
    assert(nvs["wifi_cfg"].empty() && nvs["ds_chat"].empty() && nvs["amap"].empty());
    assert(nvs["weread"].at("cookies")=="login" && nvs["weread"].count("api_key"));
    assert(nvs["stocks"].at("watchlist")=="my-list" && nvs["calibration"].at("vcom")=="1230");
    assert(!nvs["phone"].count("ringtone") && nvs["phone"].at("messages")=="history");
    assert(s_power_mode==0 && !s_wifi_initialized && !driver_live && handles.empty());
    auto at=[](const char* name){return std::find(operations.begin(),operations.end(),name);};
    assert(at("stop-before")<at("sdk-restore") && at("sdk-restore")<at("stop-after"));
    assert(at("stop-after")<at("erase:wifi_cfg") && at("commit:wifi_cfg")<at("verify:wifi_cfg/ssid"));
    assert(at("verify:ds_chat/api_key")!=operations.end() && at("verify:amap/api_key")!=operations.end());
    // SDK errors and cleanup errors must never become a successful reset.
    for(const char* stage:{"stop-before","nvs-init","net-init","rail-on","driver-init", "storage-flash","sdk-restore","storage-ram","stop-after"}) {
        fixture();failure=stage;
        assert(settings_restore_defaults()==IO_ERROR && s_power_mode==1);
        assert(nvs["wifi_cfg"].count("password") && nvs["ds_chat"].count("api_key"));
        if(failure!="stop-before")assert(deinit_calls==2);
    }
    for(const char* name:{"wifi_cfg","ds_chat","amap"}) {
        fixture();ignore_erase=name;
        assert(settings_restore_defaults()==ESP_FAIL && handles.empty());
        fixture();failure=std::string("commit:")+name;
        assert(settings_restore_defaults()==IO_ERROR && handles.empty());
        fixture();failure=std::string("verify:")+name;
        assert(settings_restore_defaults()==IO_ERROR && handles.empty());
    }
    fixture();sd_failure=true;
    assert(settings_restore_defaults()==ESP_FAIL && deinit_calls==0 && nvs["wifi_cfg"].count("password"));
    fixture();nvs.erase("wifi_cfg");nvs.erase("ds_chat");nvs.erase("amap");
    assert(settings_restore_defaults()==ESP_OK && handles.empty());
    deinit_calls=0;assert(settings_restore_defaults()==ESP_OK && handles.empty());
    std::puts("Settings reset: SDK order/errors, Wi-Fi/API erasure/readback, preserved content, absent credentials and repeated reset PASS");
}
''', encoding='utf-8')
print(out / 'test.cpp')
