"""Exercise production live-radio prebuffer bounds, delegation and cancellation."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (root / 'main/audio/audio_radio_s31.cpp').read_text(encoding='utf-8')
start = source.index('esp_gmf_err_t network_live_http_read_filter(')
end = source.index('\nesp_gmf_err_io_t network_http_source_read(', start)
program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
using esp_gmf_io_handle_t=void*;
using esp_gmf_err_t=int;
constexpr int ESP_GMF_IO_ABORT=-2;
constexpr uint32_t LIVE_HTTP_PREROLL_BYTES=32768,LIVE_HTTP_REBUFFER_BYTES=16384;
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
struct esp_gmf_io_t {
    struct {uint64_t size=0;} attr;
    struct {struct {uint32_t buffer_size=131072;} buffer_cfg;} io_cfg;
    bool _is_abort=false,_is_done=false;
    uint32_t filled=0;
};
esp_gmf_io_t io;
std::atomic<bool> s_stop{false},s_network_write_enabled{true};
bool s_network_live_primed=false;
uint64_t now_us=0;
unsigned sleeps=0,refill=256,delegates=0;
bool abort_during_wait=false;
int64_t esp_timer_get_time(){return now_us;}
void esp_gmf_io_get_db_filled_size(void* handle,uint32_t* out){*out=static_cast<esp_gmf_io_t*>(handle)->filled;}
void vTaskDelay(unsigned ms){
    assert(ms==20);now_us+=uint64_t(ms)*1000;++sleeps;io.filled+=refill;
    if(abort_during_wait)s_stop.store(true);
}
esp_gmf_err_t network_http_read_filter(void* handle,void* payload,uint32_t wanted,int ticks){
    assert(handle==&io && payload==reinterpret_cast<void*>(1) && wanted==512 && ticks==77);
    ++delegates;return 13; // Preserve the original callback result and payload ownership.
}
void reset(){io={};now_us=0;sleeps=delegates=0;refill=256;abort_during_wait=false;
    s_stop=false;s_network_write_enabled=true;s_network_live_primed=false;}
''' + source[start:end] + r'''
int run(){return network_live_http_read_filter(&io,reinterpret_cast<void*>(1),512,77);}
int main(){
    reset();assert(run()==13 && s_network_live_primed);
    assert(io.filled>=32768 && sleeps==128 && delegates==1);
    unsigned previous=sleeps;io.filled=8192;assert(run()==13 && sleeps==previous);
    io.filled=0;assert(run()==13 && io.filled>=16384 && sleeps==previous+64);
    // Both waits have fixed deadlines even if the source never supplies bytes.
    reset();refill=0;assert(run()==13 && now_us==5000000 && delegates==1);
    now_us=0;sleeps=0;assert(run()==13 && now_us==2500000 && delegates==2);
    reset();io.attr.size=512;assert(run()==13 && sleeps==0); // Small finite response bypass.
    reset();io._is_done=true;assert(run()==13 && sleeps==0); // EOF must not wait for a threshold.
    reset();io._is_abort=true;assert(run()==ESP_GMF_IO_ABORT && sleeps==0 && delegates==0);
    reset();s_network_write_enabled=false;assert(run()==ESP_GMF_IO_ABORT && sleeps==0);
    reset();abort_during_wait=true;
    assert(run()==ESP_GMF_IO_ABORT && now_us==20000 && delegates==0 && !s_network_live_primed);
    reset();io.io_cfg.buffer_cfg.buffer_size=16384;
    assert(run()==13 && io.filled==8192); // Never request a threshold larger than the ring.
    reset();io.filled=65536;assert(run()==13 && sleeps==0 && s_network_live_primed);
    puts("S31 live radio buffer: startup/rebuffer, steady-state bypass, deadlines, EOF/finite, cancellation and ring capacity PASS");
}
'''
(out / 'test.cpp').write_text(program, encoding='utf-8')
