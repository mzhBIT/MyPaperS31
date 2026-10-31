"""Prepare native tests for the production GB core, MSG pixel state and SD saves."""
from pathlib import Path
import sys
from make_emulator_test_rom import make_rom

out = Path(sys.argv[1] if len(sys.argv) > 1 else '.codex-build-cache/emulator-test')
out.mkdir(parents=True, exist_ok=True)
(out / 'esp_attr.h').write_text('#pragma once\n#define IRAM_ATTR\n', encoding='utf-8')
(out / 'unistd.h').write_text('#pragma once\n#include <io.h>\n#define fsync _commit\n#define fileno _fileno\n', encoding='utf-8')
(out / 'dirent.h').write_text(r'''
#pragma once
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#define S_ISREG(m) (((m)&_S_IFMT)==_S_IFREG)
#define S_ISDIR(m) (((m)&_S_IFMT)==_S_IFDIR)
struct dirent{char d_name[1024];};
struct DIR{intptr_t handle;_finddata_t data;bool first;dirent entry;};
static DIR* opendir(const char* path){
    DIR* d=(DIR*)calloc(1,sizeof(DIR));char pattern[2048];snprintf(pattern,sizeof(pattern),"%s/*",path);
    d->handle=_findfirst(pattern,&d->data);if(d->handle==-1){free(d);return nullptr;}d->first=true;return d;
}
static dirent* readdir(DIR* d){
    if(!d->first&&_findnext(d->handle,&d->data)!=0)return nullptr;
    d->first=false;strcpy(d->entry.d_name,d->data.name);return &d->entry;
}
static int closedir(DIR* d){int r=_findclose(d->handle);free(d);return r;}
''', encoding='utf-8')
(out / 'demo.gb').write_bytes(make_rom())
(out / 'audio.gb').write_bytes(make_rom(audio=True))
(out / 'test.c').write_text(r'''
#include "gb_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void header_checksum(unsigned char* r){
    unsigned char c=0;for(int i=0x134;i<=0x14c;++i)c=c-r[i]-1;r[0x14d]=c;
}
int main(int argc,char** argv){
    assert(argc==2);unsigned char* rom=calloc(1,32768);
    FILE* f=fopen(argv[1],"rb");assert(f);assert(fread(rom,1,32768,f)==32768);fclose(f);
    assert(emu_gb_init(rom,32768));assert(!strcmp(emu_gb_title(),"S31 DEMO"));
    size_t total_audio=0;
    for(int i=0;i<60;++i){assert(emu_gb_frame(0));assert(emu_gb_audio_frames()<=550);total_audio+=emu_gb_audio_frames();}
    assert(total_audio==60u*70224u/128u);
    const unsigned char* lcd=emu_gb_lcd();int nonzero=0;
    for(int i=0;i<5760;++i)nonzero|=lcd[i];assert(nonzero);
    size_t n=emu_gb_state_size(),bn=emu_gb_battery_size();assert(bn==8197);
    unsigned char *s=malloc(n),*b=malloc(bn),*b2=malloc(bn),expected[5760];
    int16_t expected_audio[1100];size_t expected_frames=0;
    assert(emu_gb_state_export(s,n));assert(emu_gb_battery_export(b,bn));
    for(int i=0;i<10;++i)assert(emu_gb_frame(EMU_A));
    memcpy(expected,emu_gb_lcd(),sizeof(expected));assert(emu_gb_battery_export(b2,bn));assert(memcmp(b,b2,bn));
    expected_frames=emu_gb_audio_frames();memcpy(expected_audio,emu_gb_audio(),expected_frames*4);
    assert(emu_gb_state_import(s,n));for(int i=0;i<10;++i)assert(emu_gb_frame(EMU_A));
    assert(!memcmp(expected,emu_gb_lcd(),sizeof(expected)));
    assert(emu_gb_audio_frames()==expected_frames&&!memcmp(expected_audio,emu_gb_audio(),expected_frames*4));
    assert(emu_gb_state_import(s,emu_gb_legacy_state_size()));assert(emu_gb_frame(0));
    assert(emu_gb_battery_import(b,bn));assert(emu_gb_battery_export(b2,bn));assert(!memcmp(b,b2,bn));
    assert(!emu_gb_state_import(s,n-1));s[8]=255;assert(!emu_gb_state_import(s,n));
    emu_gb_tick(10);emu_gb_close();emu_gb_close();
    rom[0x148]=255;header_checksum(rom);assert(!emu_gb_init(rom,32768));
    rom[0x148]=0;rom[0x149]=255;header_checksum(rom);assert(!emu_gb_init(rom,32768));
    rom[0x149]=2;rom[0x143]=0xc0;header_checksum(rom);assert(!emu_gb_init(rom,32768));
    rom[0x143]=0;header_checksum(rom);assert(!emu_gb_init(rom,32767));
    rom[0x14d]^=1;assert(!emu_gb_init(rom,32768));rom[0x14d]^=1;
    assert(emu_gb_init(rom,32768));emu_gb_close();
    rom[0x150]=0xd3;assert(emu_gb_init(rom,32768));assert(!emu_gb_frame(0));emu_gb_close();
    free(s);free(b);free(b2);free(rom);puts("GB execution/input/state roundtrip/battery/malformed headers PASS");
}
''', encoding='utf-8')
(out / 'video_storage.cpp').write_text(r'''
#include "VideoState.h"
#include "RomStorage.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
using namespace emulator;
int main(int argc,char** argv){
    assert(argc==2);
    for(unsigned colors=0;colors<4;++colors)for(unsigned lo=0;lo<=4;++lo)for(unsigned hi=0;hi<=4;++hi){
        for(unsigned next=0;next<4;++next){
            uint8_t state=colors|(lo<<2)|(hi<<5);
            const unsigned lc=(colors&1)==(next&1)?lo:0,hc=(colors&2)==(next&2)?hi:0;
            const unsigned expected=(lc==4?0:next&1?1:2)|(hc==4?0:next&2?4:8);
            assert(drive_pair(state,next)==expected);
            assert(state==(next|(std::min(lc+1,4u)<<2)|(std::min(hc+1,4u)<<5)));
        }
    }
    uint8_t state=0;for(int i=0;i<4;++i)assert(drive_pair(state,0)==10);assert(drive_pair(state,0)==0);
    assert(drive_pair(state,1)==1); // Only the changed pixel restarts its four fields.
    uint8_t lcd[5760]{};std::vector<uint8_t> bitmap(VideoBytes+2,0x5a);
    encode_lcd(lcd,bitmap.data()+1);for(size_t i=1;i<=VideoBytes;++i)assert(bitmap[i]==0);
    memset(lcd,0xff,sizeof(lcd));encode_lcd(lcd,bitmap.data()+1);
    for(size_t i=1;i<=VideoBytes;++i)assert(bitmap[i]==255);
    assert(bitmap.front()==0x5a&&bitmap.back()==0x5a);
    memset(lcd,0,sizeof(lcd));lcd[0]=3;encode_lcd(lcd,bitmap.data()+1);
    assert(bitmap[1+(GameWidth-1)*(VideoWidth/8)]==0xe0); // Rotation/corner.
    for(int i=0;i<16;++i){unsigned out=0;for(int p=0;p<4;++p)if(!(i&(8>>p)))out|=1<<((3-p)*2);assert(image_drive(i)==out);}
    RomStorage storage;std::string rom=std::string(argv[1])+"/demo.gb";
    uint8_t *data=nullptr;size_t size=0;uint32_t crc=0;
    assert(storage.read_rom(rom,data,size,crc));assert(size==32768);free(data);
    std::vector<uint8_t> save(8197,0x31),loaded(save.size());bool found=false;
    assert(storage.read_save(rom,crc,1,loaded.data(),loaded.size(),found)&&!found);
    assert(storage.write_save(rom,crc,1,save.data(),save.size()));
    assert(storage.read_save(rom,crc,1,loaded.data(),loaded.size(),found)&&found&&loaded==save);
    save[0]=0x32;assert(storage.write_save(rom,crc,1,save.data(),save.size()));
    {std::ofstream f(rom+".s31.sav",std::ios::binary|std::ios::trunc);f<<"broken";}
    assert(storage.read_save(rom,crc,1,loaded.data(),loaded.size(),found));assert(loaded[0]==0x31); // backup recovery
    save[0]=0x33;assert(storage.write_save(rom,crc,1,save.data(),save.size()));
    {std::ofstream f(rom+".s31.sav",std::ios::binary|std::ios::trunc);f<<"broken again";}
    assert(storage.read_save(rom,crc,1,loaded.data(),loaded.size(),found));assert(loaded[0]==0x31); // good backup retained
    assert(!storage.read_save(rom,crc^1,1,loaded.data(),loaded.size(),found));
    assert(storage.write_save(rom,crc,2,save.data(),save.size()));
    assert(!storage.read_save(rom,crc,2,loaded.data(),loaded.size()-1,found)&&found);
    std::vector<RomEntry> entries;assert(storage.list(argv[1],entries));
    assert(entries.size()==2&&entries[0].name=="audio.gb"&&entries[1].name=="demo.gb");
    std::remove((rom+".s31.sav").c_str());std::remove((rom+".s31.sav.bak").c_str());
    std::remove((rom+".s31.state").c_str());
    puts("MSG four-field state/direction/rotation/packing and SD CRC/identity/backup recovery PASS");
}
''', encoding='utf-8')
print(f'Native test sources ready in {out}')
