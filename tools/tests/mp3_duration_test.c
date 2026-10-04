#include "audio/mp3_duration.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void frame(FILE *file, bool low, bool variable, const char *tag, unsigned count) {
    // MPEG1 Layer III 128kb/s stereo at 44100, or MPEG2 64kb/s mono at 22050.
    const unsigned bytes = low ? 208 : variable ? 522 : 417;
    unsigned char data[522] = {0xff, low ? 0xf3 : 0xfb, low ? 0x80 : variable ? 0xa0 : 0x90, low ? 0xc0 : 0};
    if (tag) {
        const unsigned offset = !strcmp(tag, "VBRI") ? 36 : low ? 13 : 36;
        memcpy(data + offset, tag, 4);
        if (!strcmp(tag, "VBRI")) { data[offset+5] = 1; put32(data+offset+14,count); }
        else { put32(data+offset+4,1); put32(data+offset+8,count); }
    }
    assert(fwrite(data, 1, bytes, file) == bytes);
}
static bool cancel(void *ctx) { (void)ctx; return false; }
static FILE *new_file(void) {
    FILE *file = tmpfile(); assert(file); return file;
}
int main(void) {
    mp3_duration_info_t info;
    for (int tag = 0; tag < 3; ++tag) {
        FILE *f = new_file();
        frame(f, false, false, tag == 0 ? "Xing" : tag == 1 ? "Info" : "VBRI", 100);
        for (int i=1;i<100;++i) frame(f,false,i%2,NULL,0);
        fseek(f, 7, SEEK_SET);
        assert(mp3_duration_read(f,&info,NULL,NULL));
        assert(info.duration_ms == 2612 && info.sample_rate == 44100 && info.channels == 2);
        assert(ftell(f) == 7);
        assert(mp3_duration_read_tags(f,&info,NULL,NULL));
        assert(info.duration_ms == 2612 && info.frames_scanned == 0 && ftell(f) == 7);
        fclose(f);
    }
    FILE *f = new_file();
    unsigned char id3[10] = {'I','D','3',4,0,0x10,0,0,0x08,0}; // 1024-byte body + footer.
    fwrite(id3,1,10,f);
    unsigned char zero[1034] = {0}; fwrite(zero,1,sizeof(zero),f);
    for (int i=0;i<100;++i) frame(f,false,i%2,NULL,0);
    unsigned char tail[128] = {'T','A','G'}; fwrite(tail,1,128,f);
    assert(mp3_duration_read(f,&info,NULL,NULL));
    assert(info.duration_ms == 2612 && !strcmp(info.source,"mp3-frame-scan"));
    assert(!mp3_duration_read(f,&info,cancel,NULL)); assert(info.duration_ms == 0);
    fclose(f);
    f = new_file();
    frame(f,true,false,"Xing",100);
    for(int i=1;i<100;++i)frame(f,true,false,NULL,0);
    assert(mp3_duration_read(f,&info,NULL,NULL));
    assert(info.duration_ms==2612 && info.sample_rate==22050 && info.channels==1); fclose(f);
    f = new_file(); fwrite("ID3\4\0\0\177\177\177\177",1,10,f);
    assert(!mp3_duration_read(f,&info,NULL,NULL)); fclose(f);
    f = new_file();
    for(int i=0;i<3;++i)frame(f,false,false,NULL,0);
    fwrite("\xff\xfb\x90\0",1,4,f); // Truncated final frame is not counted.
    assert(mp3_duration_read(f,&info,NULL,NULL)); assert(info.duration_ms==78); fclose(f);
    f = new_file();
    for(int i=0;i<3;++i)frame(f,false,false,NULL,0);
    fwrite("broken",1,6,f);
    frame(f,false,false,NULL,0);
    assert(!mp3_duration_read(f,&info,NULL,NULL)); assert(info.duration_ms==0); fclose(f);
    f = new_file();
    for (int i=0;i<1000;++i) frame(f,false,i%2,NULL,0);
    assert(mp3_duration_read(f,&info,NULL,NULL));
    assert(info.duration_ms==26122 && info.frames_scanned==1000);
    assert(info.read_count < 25); // Batch ~1000 headers into sequential SD windows.
    assert(!mp3_duration_read_tags(f,&info,NULL,NULL));
    assert(!strcmp(info.failure,"no-duration-tag") && info.frames_scanned==0);
    assert(!mp3_duration_read(f,&info,cancel,NULL));
    assert(info.duration_ms==0 && !strcmp(info.failure,"budget/cancel"));
    fclose(f);
    puts("MP3 duration: tags, sequential CBR/VBR scan, ID3, MPEG2, truncated frame and cancellation passed");
}
