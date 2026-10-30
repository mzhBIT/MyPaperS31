"""Native regression for radio read observers and their pool-clone lifecycle."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (root / 'main/audio/audio_radio_s31.cpp').read_text(encoding='utf-8')

def extract(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
enum esp_gmf_err_t {ESP_GMF_ERR_OK=0,ESP_GMF_ERR_FAIL=-1};
enum esp_gmf_err_io_t {ESP_GMF_IO_OK=0,ESP_GMF_IO_ABORT=-2,ESP_GMF_IO_TIMEOUT=-3};
using esp_gmf_io_handle_t=void*;
using esp_gmf_obj_handle_t=void*;
using Filter=esp_gmf_err_t(*)(void*,void*,uint32_t,int);
struct Block {uint8_t* buf=nullptr;uint32_t buf_length=0,valid_size=0;bool is_last=false;};
struct esp_gmf_payload_t {uint8_t* buf=nullptr;uint32_t buf_length=0,valid_size=0;bool is_done=false;};
struct Bus {Block next;esp_gmf_err_io_t result=ESP_GMF_IO_OK;uint32_t filled=0,delay=0;int releases=0;};
struct Cfg {struct {Filter read_filter=nullptr;unsigned buffer_size=0;} buffer_cfg;};
struct esp_gmf_io_t {
    struct {esp_gmf_err_t(*new_obj)(void*,esp_gmf_obj_handle_t*)=nullptr;} parent;
    Cfg io_cfg;Bus* data_bus=nullptr;Block db_block;
};
struct http_io_cfg_t {Cfg io_cfg;};
uint64_t now_us=0;
int64_t esp_timer_get_time(){return now_us;}
void esp_gmf_io_get_db_filled_size(void* handle,uint32_t* size){*size=static_cast<esp_gmf_io_t*>(handle)->data_bus->filled;}
esp_gmf_err_io_t esp_gmf_db_acquire_read(Bus* bus,Block* block,uint32_t wanted,int ticks){
    assert(wanted==64 && ticks==77);now_us+=bus->delay;
    if(bus->result==ESP_GMF_IO_OK)*block=bus->next;
    return bus->result;
}
// SDK constructor deliberately drops read_filter, as the installed version does.
esp_gmf_err_t esp_gmf_io_http_init(http_io_cfg_t* cfg,void** handle){
    auto* io=new esp_gmf_io_t;io->io_cfg.buffer_cfg.buffer_size=cfg->io_cfg.buffer_cfg.buffer_size;
    *handle=io;return ESP_GMF_ERR_OK;
}
'''
program += extract('struct NetworkReadStats {') + ';\n'
program += 'NetworkReadStats s_http_read_stats,s_input_read_stats;\nFilter s_input_read_filter=nullptr;\n'
for signature in ('esp_gmf_err_t network_http_read_filter(',
                  'esp_gmf_err_t network_input_read_filter(',
                  'esp_gmf_err_t network_http_io_init('):
    program += extract(signature) + '\n'
program += r'''
esp_gmf_err_t original_hls(void*,void* payload,uint32_t wanted,int ticks){
    assert(wanted==64 && ticks==77);now_us+=800000;
    auto* load=static_cast<esp_gmf_payload_t*>(payload);load->valid_size=19;
    load->is_done=true;return ESP_GMF_ERR_OK;
}
int main(){
    uint8_t data[64];Bus bus;bus.next={data,64,31,false};bus.filled=100;bus.delay=2500;
    esp_gmf_io_t io;io.data_bus=&bus;esp_gmf_payload_t payload;
    assert(network_http_read_filter(&io,&payload,64,77)==ESP_GMF_ERR_OK);
    assert(payload.buf==data && payload.buf_length==64 && payload.valid_size==31 && !payload.is_done);
    assert(io.db_block.buf==data && io.db_block.valid_size==31); // SDK release still owns this block.
    assert(s_http_read_stats.bytes==31 && s_http_read_stats.max_wait_us==2500 && s_http_read_stats.buffer_before==100);
    bus.next.valid_size=0;bus.next.is_last=true;
    assert(network_http_read_filter(&io,&payload,64,77)==ESP_GMF_ERR_OK && payload.is_done && payload.valid_size==0);
    bus.result=ESP_GMF_IO_ABORT;
    assert(int(network_http_read_filter(&io,&payload,64,77))==int(ESP_GMF_IO_ABORT));
    bus.result=ESP_GMF_IO_TIMEOUT;
    assert(int(network_http_read_filter(&io,&payload,64,77))==int(ESP_GMF_IO_TIMEOUT));
    assert(s_http_read_stats.bytes==31);
    s_input_read_filter=original_hls;
    assert(network_input_read_filter(&io,&payload,64,77)==ESP_GMF_ERR_OK);
    assert(payload.valid_size==19 && payload.is_done && s_input_read_stats.max_wait_us==800000);
    http_io_cfg_t cfg;cfg.io_cfg.buffer_cfg.buffer_size=128*1024;
    void *first=nullptr,*clone=nullptr;
    assert(network_http_io_init(&cfg,&first)==ESP_GMF_ERR_OK);
    auto* one=static_cast<esp_gmf_io_t*>(first);
    assert(one->parent.new_obj(&cfg,&clone)==ESP_GMF_ERR_OK);
    auto* two=static_cast<esp_gmf_io_t*>(clone);
    assert(one->io_cfg.buffer_cfg.read_filter==network_http_read_filter);
    assert(two->io_cfg.buffer_cfg.read_filter==network_http_read_filter);
    assert(two->io_cfg.buffer_cfg.buffer_size==128*1024);
    delete one;delete two;
    puts("S31 radio flow: buffer ownership, EOF/abort/timeout, HLS delegation, HTTP pool clones PASS");
}
'''
(out / 'test.cpp').write_text(program, encoding='utf-8')
