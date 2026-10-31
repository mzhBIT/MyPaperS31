"""Native checks of production S31 charger ownership, routing and PCM packing."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)

def function(signature):
    s = (root/'main/audio/audio_radio_s31.cpp').read_text(encoding='utf-8')
    start = s.index(signature); end = s.index('{', start)+1; depth=1
    while depth:
        depth += (s[end]=='{')-(s[end]=='}'); end+=1
    return s[start:end]

port = (root/'main/boards/MyPaperS31/UsbAudioPort.cpp').read_text(encoding='utf-8')
port = re.sub(r'^#include ["<].*[">]\n', '', port, flags=re.M)
program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include "audio/S31UsbPcm.h"
#include "boards/MyPaperS31/ChargerState.h"
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NOT_FOUND=-2,ESP_ERR_INVALID_STATE=-3,ESP_ERR_NOT_SUPPORTED=-4;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(int){}
namespace mypaper::board::s31 { constexpr uint8_t charger_address=0x6b; bool hs=false; void usb_analog_switch_set(bool v){hs=v;} }
struct Bus {
    uint8_t regs[256]{}; int writes=0,fail_reg=-1; bool fail_always=false;
    bool readRegister(uint8_t a,uint8_t r,uint8_t *v,int,int){assert(a==0x6b);*v=regs[r];return true;}
    bool writeRegister8(uint8_t a,uint8_t r,uint8_t v,int){assert(a==0x6b);++writes;
        if(r==fail_reg){if(!fail_always)fail_reg=-1;return false;} regs[r]=v;return true;}
};
namespace m5 { Bus In_I2C; }
'''+port+r'''
namespace routing {
constexpr int AUDIO_OUTPUT_SPEAKER=0,AUDIO_OUTPUT_USB=1,AUDIO_OUTPUT_AUTO=2;
constexpr int AUDIO_PLAYBACK_PLAYING=1;
std::atomic<bool> s_force_speaker{false},s_active_usb{false},s_bt_ready{true},s_pause{false};
std::atomic<int> s_output_mode{AUDIO_OUTPUT_AUTO},s_usb_volume{20},s_state{AUDIO_PLAYBACK_PLAYING};
std::atomic<uint32_t> s_output_rate{16000},s_active_rate{16000};
bool s_usb_streaming=false,connected=true,pa=true,start_ok=true;
bool host_on=false;
using audio_output_mode_t=int;
constexpr int ESP_ERR_INVALID_ARG=-5,AUDIO_PLAYBACK_PAUSED=2;
constexpr int portMAX_DELAY=1,pdTRUE=1;
std::atomic<bool> s_initialized{true},s_network_active{false},s_usb_loss_pause{false};
std::atomic<uint32_t> s_usb_retry_ms{0};
void *s_lock=nullptr,*s_worker=(void*)1;
std::mutex s_pcm_mutex,s_usb_lifecycle_mutex;
const char *NVS_MODE_KEY="mode";
int persisted_mode=0;
void load_settings(){} void ensure_sync(){}
int xSemaphoreTake(void*,int){return pdTRUE;} void xSemaphoreGive(void*){}
void save_u8(const char*,uint8_t value){persisted_mode=value;}
int64_t esp_timer_get_time(){return 1000000;}
int ensure_usb_host(){assert(s_output_mode!=AUDIO_OUTPUT_SPEAKER);host_on=true;s_usb_retry_ms=1000;return 0;}
bool usb_audio_output_wait_connected(int){return connected;}
void usb_audio_output_deinit(){host_on=false;}
void finish_usb_host(){usb_audio_output_deinit();s_usb_retry_ms=0;}
void *s_tx=(void*)1;
uint8_t s_usb_channels=2,s_usb_bits=16,s_source_pcm_bits=16,s_usb_preferred_bits=16;
unsigned starts=0,stops=0,volume=0,rate_supported=48000,bits_supported=16,channels_supported=2;
bool usb_audio_output_is_connected(){return connected;}
bool usb_audio_output_select_rate(uint32_t,uint32_t,uint8_t c,uint8_t b,uint32_t *r){
    if(c!=channels_supported || b!=bits_supported)return false;*r=rate_supported;return true;}
int usb_audio_output_set_volume(uint8_t v){volume=v;return 0;}
int usb_audio_output_start(uint32_t r,uint8_t c,uint8_t b){assert(r==rate_supported && c==channels_supported && b==bits_supported);++starts;return start_ok ? 0 : ESP_FAIL;}
int usb_audio_output_stop(){++stops;return 0;}
void pa_set(bool v){pa=v;}
'''+function('esp_err_t select_sink_locked(')+'\n'+function('esp_err_t audio_radio_set_output_mode(')+'\n'+function('void audio_radio_service_output(')+r'''
void test(){
    assert(select_sink_locked()==0 && s_active_usb && s_active_rate==48000 && !pa && volume==20);
    assert(starts==1); assert(select_sink_locked()==0 && starts==1);
    s_output_mode=AUDIO_OUTPUT_SPEAKER;
    assert(select_sink_locked()==0 && !s_active_usb && s_active_rate==16000 && pa && stops==1);
    s_output_mode=AUDIO_OUTPUT_USB;connected=false;
    assert(select_sink_locked()==ESP_ERR_NOT_FOUND && !s_active_usb);
    s_output_mode=AUDIO_OUTPUT_AUTO;
    assert(select_sink_locked()==0 && !s_active_usb);
    connected=true;start_ok=false;
    assert(select_sink_locked()==ESP_FAIL && !s_active_usb && pa);
    start_ok=true;
    connected=true;bits_supported=32;channels_supported=1;rate_supported=44100;
    assert(select_sink_locked()==0 && s_usb_channels==1 && s_usb_bits==32 && s_active_rate==44100);
    s_force_speaker=true;
    assert(select_sink_locked()==0 && !s_active_usb);
    s_force_speaker=false;s_source_pcm_bits=24;bits_supported=24;channels_supported=2;
    assert(select_sink_locked()==0 && s_usb_bits==24 && s_usb_preferred_bits==24);
    s_source_pcm_bits=16;
    assert(select_sink_locked()==0 && s_usb_bits==24); // Packed-24-only headset fallback.
    s_force_speaker=true;
    assert(select_sink_locked()==0 && !s_active_usb);
    s_force_speaker=false;s_output_mode=AUDIO_OUTPUT_USB;connected=false;
    assert(select_sink_locked(true)==0 && !s_active_usb); // Explicit resume after unplug.

    // Exercise the real setter: mode must be visible before optional Host init.
    s_output_mode=AUDIO_OUTPUT_SPEAKER;connected=true;bits_supported=16;channels_supported=2;
    assert(audio_radio_set_output_mode(AUDIO_OUTPUT_USB)==0 && host_on && s_active_usb && persisted_mode==AUDIO_OUTPUT_USB);
    assert(audio_radio_set_output_mode(AUDIO_OUTPUT_SPEAKER)==0 && !host_on && !s_active_usb);
    connected=false;
    assert(audio_radio_set_output_mode(AUDIO_OUTPUT_USB)==ESP_ERR_NOT_FOUND && !host_on && s_output_mode==AUDIO_OUTPUT_SPEAKER);
    assert(audio_radio_set_output_mode(AUDIO_OUTPUT_AUTO)==0 && host_on && !s_active_usb);
    connected=true;audio_radio_service_output();assert(s_active_usb);
    connected=false;audio_radio_service_output();
    assert(s_pause && s_usb_loss_pause && s_state==AUDIO_PLAYBACK_PAUSED);
    connected=true;audio_radio_service_output();
    assert(!s_pause && !s_usb_loss_pause && s_state==AUDIO_PLAYBACK_PLAYING);
    s_pause=true;s_state=AUDIO_PLAYBACK_PAUSED; // User pause must survive hot-plug polling.
    audio_radio_service_output();assert(s_pause && s_state==AUDIO_PLAYBACK_PAUSED);
    assert(audio_radio_set_output_mode(AUDIO_OUTPUT_SPEAKER)==0 && !host_on);
    s_initialized=false;s_worker=nullptr;connected=false;
    assert(audio_radio_set_output_mode(AUDIO_OUTPUT_USB)==0 && host_on);
    assert(audio_radio_set_output_mode(AUDIO_OUTPUT_SPEAKER)==0 && !host_on);
}
}
int main(){
    // Real boot entry must recover retained OTG before any Host owner starts.
    m5::In_I2C.regs[0x16]=0x1f;
    m5::In_I2C.regs[0x18]=0xc4;
    m5::In_I2C.regs[0x1e]=7;
    assert(s31_charger_restore_at_boot()==ESP_OK);
    assert(m5::In_I2C.regs[0x16]==0x2c && m5::In_I2C.regs[0x18]==0x84);
    uint8_t initial[256];
    for(unsigned i=0;i<256;++i) m5::In_I2C.regs[i]=uint8_t(i*3);
    m5::In_I2C.regs[0x18]&=~0x40;m5::In_I2C.regs[0x1e]=0;
    memcpy(initial,m5::In_I2C.regs,256);
    assert(s31_usb_audio_port_acquire()==0 && mypaper::board::s31::hs);
    assert((m5::In_I2C.regs[0x18]&0x40) && !(m5::In_I2C.regs[0x16]&0x33));
    assert((m5::In_I2C.regs[0x0c]>>6)==3 && (m5::In_I2C.regs[0x0d]&31)==15);
    const auto before=m5::In_I2C.writes;
    assert(s31_usb_audio_port_acquire()==0 && m5::In_I2C.writes==before);
    s31_usb_audio_port_release();
    assert(!mypaper::board::s31::hs && !memcmp(initial,m5::In_I2C.regs,256));
    for(uint8_t source=1;source<=7;++source) {
        m5::In_I2C.regs[0x1e]=source; const int writes=m5::In_I2C.writes;
        assert(s31_usb_audio_port_acquire()==ESP_ERR_INVALID_STATE && m5::In_I2C.writes==writes);
    }
    // A reset can leave our charger in OTG while the MCU ownership flag clears.
    m5::In_I2C.regs[0x1e]=7;m5::In_I2C.regs[0x18]|=0x40;m5::In_I2C.regs[0x16]&=~0x20;
    uint8_t reclaimed[256];memcpy(reclaimed,m5::In_I2C.regs,256);
    reclaimed[0x18]&=~0x40;reclaimed[0x16]|=0x20;
    assert(s31_usb_audio_port_acquire()==ESP_OK && mypaper::board::s31::hs);
    s31_usb_audio_port_release();
    assert(s31_usb_audio_port_is_released() && !memcmp(reclaimed,m5::In_I2C.regs,256));
    for(uint8_t external=1;external<=6;++external) {
        m5::In_I2C.regs[0x1e]=external;m5::In_I2C.regs[0x18]|=0x40;
        const int writes=m5::In_I2C.writes;
        assert(s31_usb_audio_port_acquire()==ESP_ERR_INVALID_STATE && m5::In_I2C.writes==writes);
    }
    memcpy(m5::In_I2C.regs,initial,256);
    m5::In_I2C.regs[0x1e]=0;
    for(int reg : {0x16,0x0a,0x0b,0x0c,0x0d,0x18}) {
        m5::In_I2C.fail_reg=reg;
        assert(s31_usb_audio_port_acquire()==ESP_FAIL && !mypaper::board::s31::hs);
        assert(!memcmp(initial,m5::In_I2C.regs,256));
    }
    assert(s31_usb_audio_port_acquire()==0);
    m5::In_I2C.fail_always=true;m5::In_I2C.fail_reg=0x18;
    s31_usb_audio_port_release();
    assert(!s31_usb_audio_port_is_released()); // Must veto light sleep.
    assert(s31_usb_audio_port_acquire()==ESP_FAIL); // Cannot reuse a half-restored role.
    m5::In_I2C.fail_always=false;m5::In_I2C.fail_reg=-1;
    s31_usb_audio_port_release();
    assert(s31_usb_audio_port_is_released() && !memcmp(initial,m5::In_I2C.regs,256));
    using s31_usb_pcm::pack;
    const int16_t samples16[]={32767,-32768,8192,-8192};
    const auto *in=reinterpret_cast<const uint8_t *>(samples16); uint8_t buf[24]{};
    assert(pack(buf,in,2,16,2,16,65536)==8);
    const uint8_t exact16[]={255,127,0,128,0,32,0,224};assert(!memcmp(buf,exact16,8));
    assert(pack(buf,in,1,16,2,32,65536)==8);
    const uint8_t exact32[]={0,0,255,127,0,0,0,128};assert(!memcmp(buf,exact32,8));
    assert(pack(buf,in+4,1,16,1,16,65536)==2 && buf[0]==0 && buf[1]==0);
    pack(buf,in,1,16,2,16,0);for(int i=0;i<4;++i)assert(buf[i]==0);
    assert(pack(buf,in,1,16,2,24,65536)==6);
    const uint8_t expanded24[]={0,255,127,0,0,128};assert(!memcmp(buf,expanded24,6));
    const uint8_t native24[]={1,0,0,255,255,255,255,255,127,0,0,128};
    assert(pack(buf,native24,2,24,2,24,65536)==12 && !memcmp(buf,native24,12));
    const uint8_t half24[]={2,0,0,254,255,255};
    const uint8_t expected_half24[]={1,0,0,255,255,255};
    assert(pack(buf,half24,1,24,2,24,32768)==6 && !memcmp(buf,expected_half24,6));
    const uint8_t native32[]={1,0,0,0,255,255,255,255};
    assert(pack(buf,native32,1,32,2,32,65536)==8 && !memcmp(buf,native32,8));
    assert(pack(buf,native24,1,24,1,24,65536)==3 && buf[0]==0 && buf[1]==0 && buf[2]==0);
    assert(pack(buf,in,1,16,2,20,65536)==0);
    routing::test();
    puts("S31 USB audio: charger ownership/rollback, external VBUS veto, routes, stereo/mono PCM 16/24/32, low-bit preservation, gain, mute PASS");
}
'''
(out/'test.cpp').write_text(program,encoding='utf-8')
