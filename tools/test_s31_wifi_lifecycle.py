"""Generate a native test of the actual S31 Wi-Fi suspend/restart functions.

Usage: python tools/test_s31_wifi_lifecycle.py output.cpp
Compile output.cpp as C++20 and run it. SDK stubs model stop-event completion
and failures; the transition functions themselves are extracted from production.
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'main/wifi/wifi_setting.c').read_text(encoding='utf-8')
def function(name):
    match = re.search(r'(?:static )?esp_err_t ' + name + r'\([^;]*?\)\s*\{', source)
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'

program = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
using esp_err_t=int;
using EventBits_t=unsigned;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=-2,ESP_ERR_NO_MEM=-3;
constexpr bool pdTRUE=true,pdFALSE=false;
constexpr int WIFI_IF_STA=0,WIFI_MODE_STA=1,WIFI_STORAGE_RAM=0;
constexpr int WIFI_PROTOCOL_11B=1,WIFI_PROTOCOL_11G=2,WIFI_PROTOCOL_11N=4,WIFI_PROTOCOL_11AX=8;
#include "wifi/S31WifiPolicy.h"
constexpr unsigned WIFI_SETTING_CONNECTED_BIT=1,WIFI_SETTING_FAIL_BIT=2,
 WIFI_SETTING_STARTED_BIT=4,WIFI_SETTING_STOPPED_BIT=8;
constexpr int WIFI_SETTING_STATE_TIMEOUT_MS=2000;
constexpr int WIFI_SETTING_NETWORK_WIFI=0,WIFI_SETTING_NETWORK_CELLULAR=1,
 WIFI_SETTING_POWER_ON_DEMAND=0,WIFI_SETTING_POWER_ALWAYS_ON=1;
#define CONFIG_IDF_TARGET_ESP32S31 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) do{}while(0)
#define ESP_LOGE(...) do{}while(0)
#define ESP_LOGW(...) do{}while(0)
#define ESP_RETURN_ON_ERROR(expr,...) do {int ret_=(expr);if(ret_)return ret_;}while(0)
#define ESP_RETURN_ON_FALSE(expr,err,...) do {if(!(expr))return err;}while(0)
static bool s_native_stop_pending=false,s_wifi_started=false,s_wifi_initialized=false,
 s_stopping=false,s_connected=false,s_c6_powered_on=false;
static int s_retry_num=0,s_network_mode=0,s_power_mode=0;
static char s_connected_ssid[33];
static void *s_sta_netif=nullptr,*s_wifi_event_group=reinterpret_cast<void*>(1);
static unsigned bits=0;
static int inits=0,starts=0,stops=0,netifs=0,stop_result=0,power_sets=0;
static bool deliver_stop=true;
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
static void wifi_setting_wait_for_always_on_task(){}
static int wifi_setting_get_power_mode(){return s_power_mode;}
static void wifi_setting_log_heap(const char*){}
static int wifi_setting_init_nvs(){return ESP_OK;}
static int wifi_setting_network_stack_init(){return ESP_OK;}
static int wifi_setting_init_events(){return ESP_OK;}
static int wifi_setting_c6_power_on(){s_c6_powered_on=true;return ESP_OK;}
static void* esp_netif_create_default_wifi_sta(){++netifs;return reinterpret_cast<void*>(2);}
static int esp_wifi_init(wifi_init_config_t*){++inits;return ESP_OK;}
static int esp_wifi_set_storage(int){return ESP_OK;}
static int esp_wifi_set_mode(int){return ESP_OK;}
static int esp_wifi_set_protocol(int,int protocol){assert(protocol==15);return ESP_OK;}
static int esp_wifi_start(){assert(!s_native_stop_pending);++starts;bits|=WIFI_SETTING_STARTED_BIT;return ESP_OK;}
static int esp_wifi_set_max_tx_power(int power){assert(s_wifi_started && power==20);++power_sets;return ESP_OK;}
static int esp_wifi_stop(){assert(s_stopping);++stops;if(!stop_result&&deliver_stop)bits|=WIFI_SETTING_STOPPED_BIT;return stop_result;}
static void xEventGroupClearBits(void*,unsigned clear){bits&=~clear;}
static unsigned xEventGroupWaitBits(void*,unsigned,bool,bool,int){return bits;}
// Native release must not destroy its driver/netif. This fallback would fail.
static int wifi_setting_deinit(bool){assert(false);return ESP_FAIL;}
'''
for name in ('wifi_setting_wait_native_stop', 'wifi_setting_suspend_native',
             'wifi_setting_release', 'wifi_setting_init'):
    program += function(name)
program += r'''
int main(){
 assert(wifi_setting_init()==ESP_OK && inits==1 && netifs==1 && starts==1);
 assert(power_sets==1);
 assert(wifi_setting_init()==ESP_OK && power_sets==2 && starts==1);
 s_connected=true;
 assert(wifi_setting_release()==ESP_OK);
 assert(!s_wifi_started && !s_connected && s_stopping && s_wifi_initialized && s_sta_netif);
 assert(wifi_setting_release()==ESP_OK && stops==1);
 assert(wifi_setting_init()==ESP_OK && inits==1 && netifs==1 && starts==2 && !s_stopping);
 s_power_mode=WIFI_SETTING_POWER_ALWAYS_ON;
 assert(wifi_setting_release()==ESP_OK && s_wifi_started && stops==1);
 s_power_mode=WIFI_SETTING_POWER_ON_DEMAND;
 stop_result=ESP_FAIL;
 assert(wifi_setting_release()==ESP_FAIL && s_wifi_started && s_wifi_initialized);
 stop_result=ESP_OK;deliver_stop=false;
 assert(wifi_setting_release()==ESP_ERR_TIMEOUT && s_native_stop_pending);
 assert(wifi_setting_init()==ESP_ERR_TIMEOUT && starts==2 && s_stopping);
 bits|=WIFI_SETTING_STOPPED_BIT;
 assert(wifi_setting_init()==ESP_OK && starts==3 && inits==1 && netifs==1 && !s_native_stop_pending);
 assert(power_sets==4);
 deliver_stop=true;
 assert(wifi_setting_release()==ESP_OK && !s_wifi_started);
 puts("S31 Wi-Fi: suspend/resume, retained resources, always-on, stop failure/timeout/recovery PASS");
}
'''
output = Path(sys.argv[1])
output.write_text(program, encoding='utf-8')
print(output)
