"""Prepare native tests for the real APU, GB integration and speaker PCM helpers."""
from pathlib import Path
import sys

out=Path(sys.argv[1] if len(sys.argv)>1 else '.codex-build-cache/emulator-test')
out.mkdir(parents=True,exist_ok=True)
(out/'audio.cpp').write_text(r'''
#include "GameAudioPcm.h"
#include "gb_core.h"
extern "C" {
#include "vendor/minigb_apu/minigb_apu.h"
}
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
using namespace emulator;
void reg(minigb_apu_ctx& a,unsigned r,unsigned v){minigb_apu_audio_write(&a,uint16_t(r),uint8_t(v));}
void square(minigb_apu_ctx& a,unsigned freq){
    memset(&a,0,sizeof(a));minigb_apu_audio_init(&a);reg(a,0xff26,0);reg(a,0xff26,0x80);
    reg(a,0xff24,0x77);reg(a,0xff25,0x11);reg(a,0xff10,0);reg(a,0xff11,0x80);reg(a,0xff12,0xf0);
    reg(a,0xff13,freq&255);reg(a,0xff14,0x80|(freq>>8));
}
void tone(unsigned freq,unsigned low,unsigned high){
    minigb_apu_ctx a{};square(a,freq);
    std::vector<int16_t> pcm(32768*2);unsigned done=0,block=548;
    while(done<32768){unsigned n=std::min(block,32768-done);minigb_apu_audio_render(&a,pcm.data()+2*done,n);done+=n;block=1097-block;}
    unsigned crossings=0;int peak=0;
    for(unsigned i=1;i<32768;++i){if(pcm[2*i-2]<=0&&pcm[2*i]>0)++crossings;peak=std::max(peak,std::abs(int(pcm[2*i])));}
    assert(crossings>=low&&crossings<=high&&peak>1000);
    reg(a,0xff26,0);minigb_apu_audio_render(&a,pcm.data(),500);
    for(int i=0;i<1000;++i)assert(pcm[i]==0);
    std::printf("APU pulse: crossings=%u Hz peak=%d, power-off silence PASS\n",crossings,peak);
}
int main(int argc,char** argv){
    assert(argc==3);tone(1750,430,450);tone(1899,870,890);
    minigb_apu_ctx a{},b{};square(a,1750);b=a;
    std::vector<int16_t> one(2048),split(2048);
    minigb_apu_audio_render(&a,one.data(),1024);minigb_apu_audio_render(&b,split.data(),548);
    minigb_apu_audio_render(&b,split.data()+1096,476);assert(one==split);
    // All four hardware channels must synthesize nonzero PCM.
    for(unsigned channel=1;channel<=3;++channel){
        memset(&a,0,sizeof(a));minigb_apu_audio_init(&a);reg(a,0xff26,0);reg(a,0xff26,0x80);
        reg(a,0xff24,0x77);reg(a,0xff25,(1u<<channel)|(16u<<channel));
        if(channel==1){reg(a,0xff16,0x80);reg(a,0xff17,0xf0);reg(a,0xff18,0xd6);reg(a,0xff19,0x86);}
        if(channel==2){for(int i=0;i<16;++i)reg(a,0xff30+i,i*17);reg(a,0xff1a,0x80);reg(a,0xff1b,0);reg(a,0xff1c,0x20);reg(a,0xff1d,0xd6);reg(a,0xff1e,0x86);}
        if(channel==3){reg(a,0xff20,0);reg(a,0xff21,0xf0);reg(a,0xff22,0x13);reg(a,0xff23,0x80);}
        minigb_apu_audio_render(&a,one.data(),1024);
        assert(std::any_of(one.begin(),one.end(),[](int16_t v){return v!=0;}));
    }
    for(uint32_t rate:{8000u,16000u,32000u,44100u,48000u}){
        GameAudioResampler whole,parts;whole.reset(rate);parts.reset(rate);
        std::vector<int16_t> in(32768*2),expected(rate+1),actual(rate+1);
        for(size_t i=0;i<32768;++i)in[i*2]=in[i*2+1]=int16_t(i*17);
        const auto n=whole.convert(in.data(),32768,expected.data(),expected.size());assert(n==rate);
        size_t source=0,dest=0;
        while(source<32768){size_t count=std::min(size_t(549),32768-source);dest+=parts.convert(in.data()+source*2,count,actual.data()+dest,actual.size()-dest);source+=count;}
        assert(dest==n&&actual==expected);
    }
    assert(game_audio_slot(-32768,100)==INT32_MIN);assert(game_audio_slot(32767,100)==2147418112);
    assert(game_audio_slot(-32768,0)==0);assert(game_audio_slot(1234,-1)==0);
    assert(game_audio_slot(1234,120)==game_audio_slot(1234,100));
    int16_t storage[GameAudioQueue::Capacity]{};GameAudioQueue q;q.bind(storage);
    std::vector<int16_t> data(4096),popped(4096);for(int i=0;i<4096;++i)data[i]=int16_t(i);
    assert(q.push(data.data(),4096));assert(!q.push(data.data(),1));assert(q.pop(popped.data(),2049)==2049);
    assert(q.push(data.data(),2049));assert(q.pop(popped.data(),4096)==4096);
    for(int i=0;i<2047;++i)assert(popped[i]==data[2049+i]);for(int i=0;i<2049;++i)assert(popped[2047+i]==data[i]);
    q.clear();assert(!q.count());
    FILE* f=fopen(argv[1],"rb");assert(f);std::vector<uint8_t> rom(32768);assert(fread(rom.data(),1,rom.size(),f)==rom.size());fclose(f);
    assert(emu_gb_init(rom.data(),rom.size()));GameAudioResampler rs;rs.reset(16000);
    std::vector<int16_t> preview;int16_t converted[810];size_t generated=0;
    for(unsigned frame=0;frame<240;++frame){assert(emu_gb_frame(frame<120?0:EMU_A));const auto count=emu_gb_audio_frames();generated+=count;
        const auto n=rs.convert(emu_gb_audio(),count,converted,810);preview.insert(preview.end(),converted,converted+n);}
    assert(generated==240u*70224u/128u);assert(preview.size()==generated*16000/32768);
    assert(std::any_of(preview.begin(),preview.end(),[](int16_t v){return v!=0;}));
    f=fopen(argv[2],"wb");assert(f);assert(fwrite(preview.data(),2,preview.size(),f)==preview.size());fclose(f);
    emu_gb_close();puts("All-channel APU, chunk invariance, rate conversion, FIFO wrap/full, slot packing and GB sound ROM PASS");
}
''',encoding='utf-8')
print(f'Audio test source ready: {out / "audio.cpp"}')
