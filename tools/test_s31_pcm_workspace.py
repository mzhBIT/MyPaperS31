"""Exercise production PCM writes with a reused PSRAM chunk and bounded sinks."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
source=(root/'main/audio/audio_radio_s31.cpp').read_text(encoding='utf-8')
start=source.index('bool write_pcm(');end=source.index('{',start)+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
program=r'''
#include "audio/S31UsbPcm.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
constexpr size_t WRITE_CHUNK_FRAMES=512,PCM_WORKSPACE_BYTES=4096;
constexpr int USB_VOLUME_CEILING=5,ESP_OK=0,ESP_ERR_INVALID_STATE=-1,ESP_ERR_NOT_FOUND=-2;
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,AUDIO_PLAYBACK_PAUSED=2;
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
using esp_err_t=int;
uint8_t *s_pcm_workspace=nullptr;
std::atomic<bool> s_active_usb{true},s_stop{false},s_pause{false},s_usb_loss_pause{false};
std::atomic<int> s_usb_volume{100},s_state{1};
std::atomic<unsigned> s_active_rate{48000};
std::atomic<uint64_t> s_played_frames{0},s_played_time_us{0};
bool s_usb_streaming=true,connected=true,fail_allocation=false;
uint8_t s_usb_channels=2,s_usb_bits=24;
unsigned allocations=0;
std::vector<uint8_t> usb;
std::vector<int32_t> speaker;
unsigned uxTaskGetStackHighWaterMark(void*){return 5000;}
void *heap_caps_malloc(size_t bytes,int caps){assert(bytes==4096 && caps==3);if(fail_allocation)return nullptr;++allocations;return malloc(bytes);}
bool usb_audio_output_uses_software_volume(){return false;}
bool usb_audio_output_is_connected(){return connected;}
int volume_factor(){return 65536;}
esp_err_t usb_audio_output_write(const uint8_t *p,size_t bytes,unsigned){
    assert(p==s_pcm_workspace && bytes<=PCM_WORKSPACE_BYTES);usb.insert(usb.end(),p,p+bytes);return ESP_OK;
}
bool i2s_write_all(const void *p,size_t bytes){
    assert(p==s_pcm_workspace && bytes<=PCM_WORKSPACE_BYTES);
    auto *values=static_cast<const int32_t*>(p);speaker.insert(speaker.end(),values,values+bytes/4);return true;
}
'''+source[start:end]+r'''
int main(){
    const uint8_t sample24[]={1,0,0,255,255,255};
    std::vector<uint8_t> input;for(int i=0;i<1100;++i)input.insert(input.end(),sample24,sample24+6);
    assert(write_pcm(input.data(),1100,24));assert(usb==input && allocations==1 && s_played_frames==1100);
    const auto *workspace=s_pcm_workspace;
    s_active_usb=false;s_active_rate=16000;
    const int16_t samples[]={32767,-32768,-32768,32767};
    assert(write_pcm(reinterpret_cast<const uint8_t*>(samples),2,16));
    assert(allocations==1 && workspace==s_pcm_workspace && speaker.size()==4);
    assert(speaker[0]==0x7fff0000 && speaker[1]==0x7fff0000);
    assert(speaker[2]==INT32_MIN && speaker[3]==INT32_MIN);
    assert(!write_pcm(input.data(),1,24)); // Speaker cannot receive packed-24.
    s_active_usb=true;connected=false;
    assert(write_pcm(input.data(),1,24) && s_pause && s_usb_loss_pause && s_state==AUDIO_PLAYBACK_PAUSED);
    free(s_pcm_workspace);s_pcm_workspace=nullptr;fail_allocation=true;
    assert(!write_pcm(input.data(),1,24) && !s_pcm_workspace && allocations==1);
    puts("S31 PCM workspace: PSRAM allocation, USB multi-chunk native24, speaker layout, reuse, unplug pause, allocation failure PASS");
}
'''
(out/'test.cpp').write_text(program,encoding='utf-8')
