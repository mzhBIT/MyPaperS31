"""Native regression of production radio/power-action sequencing (SDK IO stubs).

Generate a C++20 source and stub headers under the supplied output directory.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
(out / 'M5GFX.h').write_text('''#pragma once
namespace lgfx { inline unsigned tick=1000; inline unsigned millis(){return tick;} }
''')
(out / 'esp_log.h').write_text('''#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
''')

def function(path, signature):
    text = (root / path).read_text(encoding='utf-8')
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

program = r'''
#include "activities/ActivityManager.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, portMAX_DELAY=-1;
constexpr int AUDIO_PLAYBACK_STOPPED=1, AUDIO_PLAYBACK_ERROR=5;
constexpr int CONFIG_WIFI_SETTING_CONNECT_TIMEOUT_MS=30000;
std::vector<std::string> events;
bool http_alive=true, wifi_alive=true, pa_on=true, join_ok=true, keys_ok=true;
bool touch_ok=true;
unsigned touch_pauses=0,touch_resumes=0,key_resumes=0;
int wifi_stops=0, enters=0, sink_closes=0, connect_result=ESP_OK;
std::atomic<bool> s_initialized{true};
std::atomic<int> s_state{2};
int s_lock=1;
void ensure_sync(){}
void xSemaphoreTake(int,int){}
void xSemaphoreGive(int){}
void pa_set(bool on){assert(!on);events.push_back("mute");pa_on=false;}
bool join_worker_locked(){
    assert(wifi_alive && !pa_on);events.push_back("join");
    if(!join_ok)return false;
    http_alive=false;return true;
}
void audio_radio_deinit(){
    assert(!http_alive && !pa_on);events.push_back("sink-off");
    ++sink_closes;s_initialized=false;
}
'''
program += function('main/audio/audio_radio_s31.cpp', 'esp_err_t audio_radio_stop(void)')
program += r'''
struct RadioPage : Activity {
    void* _play_task=nullptr;
    bool _switching=true, _error_drawn=true;
    int _queued_station=3, _queued_request_id=5;
    std::string _status="Starting";
    void on_create() override {}
    bool can_standby() const override { return _play_task==nullptr; }
    void on_hide() override { ++enters;events.push_back("enter"); }
'''
program += function('main/activities/RadioActivity.cpp', 'bool prepare_standby() override')
program += r'''
};
struct {struct {bool displayBusy(){return false;}} Display;} M5;
bool touch_input_needs_fast_poll(){return false;}
bool wifi_setting_background_busy(){return false;}
bool network_time_busy(){return false;}
bool network_time_try_pause(){return true;}
void network_time_resume(){}
namespace weread { std::mutex mutex;std::mutex& reading_transaction_mutex(){return mutex;} }
namespace keys {
bool pause_for_sleep(){return keys_ok;}
void resume_after_sleep(){++key_resumes;}
}
bool touch_input_pause_for_sleep(){++touch_pauses;return touch_ok;}
void touch_input_resume_after_sleep(){++touch_resumes;}
int cellular_network_suspend(){return ESP_OK;}
bool wifi_setting_is_enabled(){return wifi_alive;}
bool wifi_setting_is_connected(){return wifi_alive;}
int wifi_setting_deinit(bool){
    assert(!http_alive && !pa_on && !s_initialized);
    events.push_back("netif-off");wifi_alive=false;++wifi_stops;return ESP_OK;
}
bool pending=true,shutdown_requested=false;
unsigned requested_at=1000,generation=0;
'''
standby = (root / 'main/boards/MyPaperS31/StandbyS31.cpp').read_text(encoding='utf-8')
start = standby.index('bool service() {')
end = standby.index('    using Buffer', start)
program += standby[start:end]
program += '\n    return ret==ESP_OK;\n}\n'
program += r'''
struct RadioPlayResult {int index;unsigned request_id;int result;};
struct RadioPlayJob {void* result_queue;int index;unsigned request_id;const char* url;};
RadioPlayResult last_result{};
int wifi_setting_connect_saved(int){
    events.push_back("connect");if(connect_result==ESP_OK)wifi_alive=true;return connect_result;
}
int audio_radio_play(const char*){assert(wifi_alive);events.push_back("play");return ESP_OK;}
int xQueueSend(void*,const RadioPlayResult* result,int){last_result=*result;return 1;}
void vTaskDeleteWithCaps(void*){}
'''
program += function('main/activities/RadioActivity.cpp', 'static void radio_play_worker(void *arg)')
program += r'''
int main(){
    auto& am=ActivityManager::instance();auto* page=new RadioPage;am.push(page);lgfx::tick+=200;
    auto reset=[&]{
        events.clear();http_alive=wifi_alive=pa_on=join_ok=true;s_initialized=true;
        wifi_stops=enters=sink_closes=0;pending=true;shutdown_requested=false;
        touch_ok=true;touch_pauses=touch_resumes=key_resumes=0;
        generation=am.generation();requested_at=lgfx::tick;
    };
    for(bool shutdown : {false,true}) {
        reset();shutdown_requested=shutdown;
        assert(service());
        assert((events==std::vector<std::string>{"mute","join","sink-off","enter","netif-off"}));
        assert(wifi_stops==1 && !http_alive && !pending);
        assert(touch_pauses==1 && touch_resumes==1);
        assert(!page->_switching && page->_queued_station==-1 && page->_status.empty());
    }
    reset();touch_ok=false;
    assert(service() && wifi_alive && wifi_stops==0);
    assert(touch_pauses==1 && touch_resumes==0 && key_resumes==1);
    reset();join_ok=false;
    assert(service());
    assert((events==std::vector<std::string>{"mute","join"}));
    assert(wifi_alive && http_alive && wifi_stops==0 && enters==0 && sink_closes==0);
    reset();page->_play_task=reinterpret_cast<void*>(1);
    assert(!service() && pending && events.empty() && wifi_alive);
    page->_play_task=nullptr;
    events.clear();wifi_alive=false;
    radio_play_worker(new RadioPlayJob{nullptr,4,9,"http://station"});
    assert((events==std::vector<std::string>{"connect","play"}));
    assert(last_result.index==4 && last_result.request_id==9 && last_result.result==ESP_OK);
    events.clear();
    radio_play_worker(new RadioPlayJob{nullptr,5,10,"http://another-station"});
    assert((events==std::vector<std::string>{"play"})); // Connected switch must not reconfigure Wi-Fi.
    events.clear();connect_result=ESP_FAIL;wifi_alive=false;
    radio_play_worker(new RadioPlayJob{nullptr,4,10,"http://station"});
    assert((events==std::vector<std::string>{"connect"}) && last_result.result==ESP_FAIL);
    am.pop();
    puts("Radio standby: mute/join/sink/netif order, standby/shutdown, failure veto, startup deferral, wake reconnect PASS");
}
'''
(out / 'test.cpp').write_text(program, encoding='utf-8')
