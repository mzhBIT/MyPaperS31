"""Native regression for production S31 radio resampling hooks and idle budget."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (root / 'main/audio/audio_radio_s31.cpp').read_text(encoding='utf-8')
start = source.index('esp_gmf_job_func s_network_rate_open')
end = source.index('\nint network_write(', start)
program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
using esp_gmf_job_err_t = int;
using esp_gmf_job_func = int(*)(void*,void*);
constexpr int ESP_AE_RATE_CVT_PERF_TYPE_SPEED=1;
struct esp_ae_rate_cvt_cfg_t {
    uint32_t src_rate=44100,dest_rate=16000;
    unsigned channel=2,bits_per_sample=16,complexity=2,perf_type=0;
};
#define OBJ_GET_CFG(self) (self)
uint64_t now_us=0;
unsigned logs=0,yields=0,opens=0,processes=0;
int open_result=0,process_result=0;
#define ESP_LOGI(...) (++logs)
int64_t esp_timer_get_time(){return now_us;}
void vTaskDelay(unsigned ticks){assert(ticks==1);++yields;now_us+=10000;}
int sdk_open(void* self,void* context){
    assert(context==reinterpret_cast<void*>(1));++opens;
    auto* cfg=static_cast<esp_ae_rate_cvt_cfg_t*>(self);
    assert(cfg->complexity==1 && cfg->perf_type==ESP_AE_RATE_CVT_PERF_TYPE_SPEED);
    assert(cfg->dest_rate==16000 && cfg->bits_per_sample==16);
    return open_result;
}
int sdk_process(void* self,void* context){
    assert(context==reinterpret_cast<void*>(1));++processes;
    auto* cfg=static_cast<esp_ae_rate_cvt_cfg_t*>(self);
    cfg->src_rate=48000; // SDK format reopen must update the timing rate.
    now_us+=140000;
    return process_result;
}
''' + source[start:end] + r'''
int main(){
    esp_ae_rate_cvt_cfg_t cfg;
    s_network_rate_open=sdk_open;s_network_rate_process=sdk_process;
    auto* context=reinterpret_cast<void*>(1);
    assert(network_rate_open(&cfg,context)==0 && opens==1);
    open_result=-1;
    assert(network_rate_open(&cfg,context)==-1 && opens==2);
    process_result=-2;
    assert(network_rate_process(&cfg,context)==-2 && processes==1);
    assert(s_network_rate_timing.source==48000 && s_network_rate_timing.destination==16000);
    assert(s_network_rate_timing.blocks==1 && s_network_rate_timing.max_us==140000);
    assert(s_network_rate_timing.total_us==140000);
    network_pcm_checkpoint();assert(yields==0);
    now_us=250000;network_pcm_checkpoint();assert(yields==1);
    network_pcm_checkpoint();assert(yields==1); // No unconditional per-block sleep.
    now_us=5000000;unsigned old_logs=logs;
    network_pcm_checkpoint();assert(yields==2 && logs==old_logs+1);
    assert(s_network_rate_timing.blocks==0 && s_network_rate_timing.max_us==0);
    assert(s_network_rate_timing.total_us==0);
    // Millisecond wrap must not suppress the idle opportunity for 49 days.
    s_network_rate_timing.last_yield_ms=UINT32_MAX-100;
    now_us=(uint64_t(UINT32_MAX)+201)*1000;
    network_pcm_checkpoint();assert(yields==3);
    puts("S31 radio schedule: lower-cost filtered SRC, SDK result/format propagation, bounded idle yield, timing reset/wrap PASS");
}
'''
(out / 'test.cpp').write_text(program, encoding='utf-8')
# Ensure the idle checkpoint stays after PCM submission and outside its mutex.
write = source[source.index('int network_write('):source.index('int network_prepare(')]
assert write.index('write_pcm(') < write.index('if (ok) network_pcm_checkpoint();')
assert write.index('if (ok) network_pcm_checkpoint();') < write.rindex('s_network_last_pcm_ms.store(')
assert 'taskYIELD();' not in write
