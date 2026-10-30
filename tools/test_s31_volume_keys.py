"""Generate a native regression using actual S31 volume settings/key dispatch.

SDK NVS/IO are stubs; production code is extracted from the firmware sources.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)

def function(path, signature):
    source = (root / path).read_text(encoding='utf-8')
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include "input/BoardKeyState.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
using namespace board_keys;
using esp_err_t=int;
using nvs_handle_t=unsigned;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=-1;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
constexpr int AUDIO_OUTPUT_SPEAKER=0,AUDIO_OUTPUT_USB=1,AUDIO_OUTPUT_AUTO=2;
const char* NVS_NAMESPACE="audio_out";
const char* NVS_SPK_VOLUME_KEY="spk_volume";
const char* NVS_USB_VOLUME_KEY="usb_volume";
const char* NVS_MODE_KEY="mode";
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
std::atomic<int> s_volume{30},s_output_mode{0},s_usb_volume{20};
std::atomic<bool> s_active_usb{false},s_force_speaker{false},s_initialized{false};
int usb_audio_output_set_volume(uint8_t){return 0;}
std::mutex s_settings_mutex;
bool s_settings_loaded=false;
unsigned reads=0,commits=0,audio_inits=0;
uint8_t saved_volume=80;
int nvs_open(const char*,int mode,nvs_handle_t* out){*out=1;if(mode==NVS_READONLY)++reads;return ESP_OK;}
int nvs_get_u8(unsigned,const char* key,uint8_t* value){
    if (!std::strcmp(key,NVS_USB_VOLUME_KEY)) return -1;
    *value=std::strcmp(key,NVS_SPK_VOLUME_KEY)==0 ? saved_volume : 0;return ESP_OK;
}
int nvs_set_u8(unsigned,const char* key,uint8_t value){assert(!std::strcmp(key,NVS_SPK_VOLUME_KEY));saved_volume=value;return ESP_OK;}
int nvs_commit(unsigned){++commits;return ESP_OK;}
void nvs_close(unsigned){}
int audio_radio_init(){++audio_inits;return ESP_OK;}
'''
for signature in ('void load_settings()', 'void save_u8(',
                  'esp_err_t audio_radio_set_volume(', 'int audio_radio_get_volume('):
    program += function('main/audio/audio_radio_s31.cpp', signature) + '\n'
program += r'''
struct Page {
    bool handles=false;
    int calls=0,last_delta=0;
    bool on_volume_key(int delta){++calls;last_delta=delta;return handles;}
} page;
void dispatch(Key key,Gesture gesture){
    struct {Key key;Gesture gesture;} event{key,gesture};
    auto* activity=&page;
    switch(key){
'''
source = (root / 'main/boards/MyPaperS31/KeysS31.cpp').read_text(encoding='utf-8')
start = source.index('        case Key::VolumeUp: case Key::VolumeDown: {')
end = source.index('        default: break;', start)
program += source[start:end]
program += r'''
    default: break;
    }
}
int main(){
    assert(audio_radio_get_volume()==80 && reads==1 && audio_inits==0);
    for(int i=0;i<100;++i) assert(audio_radio_get_volume()==80);
    assert(reads==1);
    dispatch(Key::VolumeUp,Gesture::Click);assert(saved_volume==90);
    load_settings();assert(audio_radio_get_volume()==90 && reads==1);
    dispatch(Key::VolumeUp,Gesture::Click);assert(saved_volume==100);
    const auto before=commits;
    dispatch(Key::VolumeUp,Gesture::Click);assert(commits==before);
    dispatch(Key::VolumeDown,Gesture::Click);assert(saved_volume==90);
    dispatch(Key::VolumeDown,Gesture::LongPress);assert(saved_volume==0);
    dispatch(Key::VolumeUp,Gesture::LongPress);assert(saved_volume==100);
    page.handles=true;
    const auto handled_before=commits;
    dispatch(Key::VolumeDown,Gesture::Click);
    assert(page.last_delta==-10 && commits==handled_before);
    const auto calls=page.calls;
    dispatch(Key::VolumeUp,Gesture::None);assert(page.calls==calls);
    page.handles=false;
    dispatch(Key::VolumeDown,Gesture::LongPress);
    s_settings_loaded=false;s_volume=30; // Reboot with a saved mute setting.
    assert(audio_radio_get_volume()==0 && audio_inits==0);
    assert(audio_radio_set_volume(-1)==ESP_ERR_INVALID_ARG);
    assert(audio_radio_set_volume(101)==ESP_ERR_INVALID_ARG);
    puts("S31 volume: saved level, one-time load, global/app dispatch, limits, mute/max, persistence, no audio startup PASS");
}
'''
(out / 'test.cpp').write_text(program, encoding='utf-8')
