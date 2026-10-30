"""Exercise production converter configuration and byte budgets with SDK stubs."""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
source=(root/'main/audio/audio_radio_s31.cpp').read_text(encoding='utf-8')
source=source[source.index('struct Converters {'):source.index('// ------------------------------------------------------------------- sink ---')]
source=source.replace('realloc(', 'test_realloc(').replace('free(', 'test_free(')
program=r'''
#include "audio/S31UsbPcm.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
constexpr int ESP_AE_ERR_OK=0,ESP_AE_RATE_CVT_PERF_TYPE_MEMORY=0;
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
using esp_ae_sample_t=void*;
struct Stage {unsigned source_bits,dest_bits,channels;};
using esp_ae_bit_cvt_handle_t=Stage*;
using esp_ae_ch_cvt_handle_t=Stage*;
using esp_ae_rate_cvt_handle_t=Stage*;
struct esp_ae_bit_cvt_cfg_t {uint32_t sample_rate;uint8_t channel,src_bits,dest_bits;};
struct esp_ae_ch_cvt_cfg_t {uint32_t sample_rate;uint8_t bits_per_sample,src_ch,dest_ch;void* weight;unsigned weight_len;};
struct esp_ae_rate_cvt_cfg_t {uint32_t src_rate,dest_rate;uint8_t channel,bits_per_sample,complexity;int perf_type;};
std::unordered_map<void*,size_t> sizes;
bool fail_open=false;
void *test_realloc(void *p,size_t bytes){void *n=realloc(p,bytes);if(n){sizes.erase(p);sizes[n]=bytes;}return n;}
void test_free(void *p){sizes.erase(p);free(p);}
void enough(void *p,size_t bytes){assert(sizes.count(p) && sizes.at(p)>=bytes);}
int esp_ae_bit_cvt_open(esp_ae_bit_cvt_cfg_t *c,Stage **h){if(fail_open){fail_open=false;return -1;}*h=new Stage{c->src_bits,c->dest_bits,c->channel};return 0;}
int esp_ae_ch_cvt_open(esp_ae_ch_cvt_cfg_t *c,Stage **h){assert(c->dest_ch==2);*h=new Stage{c->bits_per_sample,c->bits_per_sample,c->src_ch};return 0;}
int esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t *c,Stage **h){assert(c->channel==2);*h=new Stage{c->bits_per_sample,c->bits_per_sample,2};return 0;}
void esp_ae_bit_cvt_close(Stage *h){delete h;} void esp_ae_ch_cvt_close(Stage *h){delete h;} void esp_ae_rate_cvt_close(Stage *h){delete h;}
int esp_ae_bit_cvt_process(Stage *h,unsigned frames,void *input,void *output){
    enough(output,size_t(frames)*h->channels*(h->dest_bits/8));
    auto *in=static_cast<uint8_t*>(input);auto *out=static_cast<uint8_t*>(output);
    for(unsigned i=0;i<frames*h->channels;++i){
        int64_t v=s31_usb_pcm::sample(in+i*(h->source_bits/8),h->source_bits);
        if(h->dest_bits>h->source_bits)v*=int64_t(1)<<(h->dest_bits-h->source_bits);
        else if(h->dest_bits<h->source_bits)v/=int64_t(1)<<(h->source_bits-h->dest_bits);
        for(unsigned b=0;b<h->dest_bits/8;++b)out[i*(h->dest_bits/8)+b]=uint8_t(uint32_t(v)>>(b*8));
    }return 0;
}
int esp_ae_ch_cvt_process(Stage *h,unsigned frames,void *input,void *output){
    assert(h->channels==1);const unsigned bytes=h->dest_bits/8;enough(output,size_t(frames)*2*bytes);
    auto *in=static_cast<uint8_t*>(input);auto *out=static_cast<uint8_t*>(output);
    for(unsigned i=0;i<frames;++i)for(unsigned c=0;c<2;++c)memcpy(out+(i*2+c)*bytes,in+i*bytes,bytes);
    return 0;
}
int esp_ae_rate_cvt_get_max_out_sample_num(Stage*,unsigned count,unsigned *out){*out=count*2;return 0;}
int esp_ae_rate_cvt_process(Stage *h,void *input,unsigned count,void *output,unsigned *out_count){
    const unsigned bytes=2*(h->dest_bits/8);enough(output,size_t(count)*2*bytes);
    for(unsigned i=0;i<count;++i)for(unsigned n=0;n<2;++n)memcpy(static_cast<uint8_t*>(output)+(i*2+n)*bytes,static_cast<uint8_t*>(input)+i*bytes,bytes);
    *out_count=count*2;return 0;
}
'''+source+r'''
int main(){
    Converters c;const uint8_t *out=nullptr;uint32_t frames=0;
    const uint8_t native24[]={1,0,0,255,255,255};
    assert(c.configure(48000,2,24,48000,24));
    assert(!c.bit && !c.channel && !c.rate);
    assert(c.process(native24,sizeof(native24),&out,&frames) && out==native24 && frames==1);
    assert(c.configure(44100,1,24,48000,24));
    assert(!c.bit && c.channel && c.rate && c.channel->dest_bits==24 && c.rate->dest_bits==24);
    assert(c.process(native24,sizeof(native24),&out,&frames) && frames==4);
    assert(c.cap2>=12 && c.cap3>=24);
    assert(out[0]==1 && out[3]==1 && out[12]==255 && out[15]==255);
    assert(c.configure(48000,2,16,48000,24));
    const uint8_t in16[]={1,0,255,255};
    assert(c.process(in16,sizeof(in16),&out,&frames) && frames==1 && c.cap1>=6);
    const uint8_t expanded[]={0,1,0,0,255,255};assert(!memcmp(out,expanded,6));
    assert(c.configure(48000,2,32,48000,32));
    assert(!c.bit && !c.channel && !c.rate);
    assert(c.configure(48000,2,24,48000));assert(c.dst_bits==16); // Speaker default unchanged.
    fail_open=true;
    assert(!c.configure(48000,2,16,48000,24) && !c.configured);
    assert(c.configure(48000,2,16,48000,24) && c.configured);
    c.close();assert(sizes.empty());
    puts("S31 converters: native 24/32-bit, mono/rate stages, byte budgets, 16->24, speaker default, failed-open retry PASS");
}
'''
(out/'test.cpp').write_text(program,encoding='utf-8')
