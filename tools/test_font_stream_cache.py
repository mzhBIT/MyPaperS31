"""Generate a native lifecycle test using the production cache functions."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'main/renderer/TextRenderer.cpp').read_text(encoding='utf-8')

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <limits>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <cerrno>
#define open(path,flags) _open(path,(flags)|_O_BINARY)
using ssize_t=int;
static int64_t esp_timer_get_time(){return 0;}
constexpr int FONT_STREAM_MIN_WINDOWS=4;
constexpr int FONT_STREAM_WINDOW_COUNT=64;
constexpr size_t FONT_STREAM_WINDOW_BYTES=16384;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
'''
program += source[source.index('struct FontCacheWindow {'):source.index('static uint8_t *allocate_font_stream_window')]
program += r'''
static int live=0, allocation_budget=64;
static std::unordered_set<void*> allocations;
static uint8_t* allocate_font_stream_window(size_t size,bool* dma,bool* psram,size_t* alignment){
    if (!allocation_budget) return nullptr;
    --allocation_budget; ++live;
    *dma=false; *psram=true; *alignment=64;
    auto* p=static_cast<uint8_t*>(std::malloc(size)); allocations.insert(p); return p;
}
static void heap_caps_free(void* p){assert(p);if(allocations.erase(p))--live;std::free(p);}
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;
static void* heap_caps_calloc(size_t n,size_t size,int){return std::calloc(n,size);}
namespace font_stream_limits { template<class... Args> bool supported(Args...){return true;} }
struct FT_StreamRec;
using FT_Stream=FT_StreamRec*;
using FT_Long=long;
struct FT_StreamRec {
    unsigned long size=0,pos=0;
    struct {void* pointer=nullptr;} descriptor,pathname;
    unsigned long (*read)(FT_Stream,unsigned long,unsigned char*,unsigned long)=nullptr;
    void (*close)(FT_Stream)=nullptr;
};
struct TextRenderer { void* _font_stream_ctx=nullptr; void releaseFontStreamCache(); };
'''
program += function('static unsigned long read_font_file_range(')
program += function('static unsigned font_stream_cache_bytes(')
program += function('static FontCacheWindow *pick_font_cache_window(')
program += function('static void restore_font_stream_cache(')
program += function('static unsigned long font_stream_io(')
program += function('static void font_stream_close(')
program += function('static bool create_font_stream(')
program += function('void TextRenderer::releaseFontStreamCache(')
program += function('static void destroy_font_stream(')
program += r'''
int main(){
    TextRenderer renderer;
    renderer.releaseFontStreamCache(); // Embedded font: no context.
    FontStreamContext ctx;
    renderer._font_stream_ctx=&ctx;
    ctx.requested_windows=8;
    ctx.cache_released=true;
    restore_font_stream_cache(&ctx);
    assert(live==8 && ctx.allocated_windows==8);
    assert(font_stream_cache_bytes(&ctx)==8*16384);
    ctx.windows[0].size=100; ctx.windows[0].start=4096;
    renderer.releaseFontStreamCache();
    assert(!live && !ctx.allocated_windows && ctx.cache_released);
    for(const auto& w:ctx.windows) assert(!w.data && !w.size && !w.start && !w.last_used);
    renderer.releaseFontStreamCache(); // Repeated exit must not double-free.
    allocation_budget=2;
    restore_font_stream_cache(&ctx);
    assert(live==2 && ctx.allocated_windows==2 && !ctx.cache_released);
    restore_font_stream_cache(&ctx); // Normal reads must not reallocate.
    assert(live==2);
    renderer.releaseFontStreamCache();
    allocation_budget=0;
    restore_font_stream_cache(&ctx);
    assert(!live && !ctx.allocated_windows && !ctx.cache_released);
    renderer.releaseFontStreamCache();
    allocation_budget=64;
    restore_font_stream_cache(&ctx);
    assert(live==8);
    renderer.releaseFontStreamCache();
    assert(!live);
    const char* file=".codex-build-cache/font-cache-fixture.bin";
    { auto* fp=std::fopen(file,"wb"); assert(fp); for(int i=0;i<32768;++i) std::fputc(i&255,fp);std::fclose(fp); }
    void *s1=nullptr,*c1=nullptr,*s2=nullptr,*c2=nullptr,*s3=nullptr,*c3=nullptr;
    allocation_budget=64;
    assert(create_font_stream(file,&s1,&c1,4,true));
    assert(create_font_stream(file,&s2,&c2,4,true));
    assert(c1==c2 && s1!=s2 && live==4);
    auto* shared=static_cast<FontStreamContext*>(c1);
    assert(shared->references==2 && shared_font_streams.size()==1);
    uint8_t bytes[32];
    assert(font_stream_io(static_cast<FT_Stream>(s1),100,bytes,32)==32 && bytes[0]==100);
    auto reads=shared->physical_reads;
    assert(font_stream_io(static_cast<FT_Stream>(s2),100,bytes,32)==32 && shared->physical_reads==reads);
    assert(create_font_stream(file,&s3,&c3,4,false));
    assert(c3!=c1 && live==8); // Reader stream remains independent.
    destroy_font_stream(s1,c1);
    assert(shared->references==1 && live==8);
    renderer._font_stream_ctx=c2;
    renderer.releaseFontStreamCache();
    assert(live==4);
    assert(font_stream_io(static_cast<FT_Stream>(s2),100,bytes,32)==32 && live==8);
    destroy_font_stream(s2,c2);
    assert(shared_font_streams.empty() && live==4);
    destroy_font_stream(s3,c3);
    assert(!live);
    allocation_budget=1;
    assert(!create_font_stream(file,&s1,&c1,4,true));
    assert(!live && !s1 && !c1 && shared_font_streams.empty());
    std::remove(file);
    puts("Font stream cache: release/repeat/resume/partial allocation/no memory PASS");
    puts("Shared UI stream: one file/cache, cross-stream hits, reference lifetime, private reader PASS");
}
'''
Path(sys.argv[1]).write_text(program, encoding='utf-8')
