"""Extract production UI pool growth/budget code for native regression."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
renderer=(root/'main/renderer/TextRenderer.cpp').read_text(encoding='utf-8')
ui=(root/'main/ui/SystemFont.cpp').read_text(encoding='utf-8')
def function(source, signature):
    start=source.index(signature)
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]+'\n'
program=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
static bool fail_alloc=false;
static void* heap_caps_realloc(void* p,size_t n,int){return fail_alloc?nullptr:std::realloc(p,n);}
struct TextRenderer {
    struct {int glyph_pool_initial_bytes=8192,glyph_pool_bytes=131072;
        bool (*allow_glyph_pool_growth)(void*,size_t)=nullptr;void* glyph_pool_owner=nullptr;} _style;
    uint8_t* _glyph_pool=nullptr;
    size_t _glyph_pool_used=0,_glyph_pool_capacity=0;
    struct Stats {size_t glyph_pool_capacity;};
    Stats stats()const{return {_glyph_pool_capacity};}
    void end(){std::free(_glyph_pool);_glyph_pool=nullptr;_glyph_pool_used=_glyph_pool_capacity=0;}
    void init(size_t cap){end();_glyph_pool=static_cast<uint8_t*>(std::malloc(cap));_glyph_pool_capacity=cap;}
    bool ensureGlyphPool(size_t);
};
struct Bank {
    int px=0;uint32_t last_used=0;
    TextRenderer primary,builtin,fallback;
    void clear(){primary.end();builtin.end();fallback.end();px=0;}
};
static std::array<Bank,12> banks;
constexpr size_t UI_GLYPH_POOL_BUDGET=768*1024;
'''
program+=function(ui,'size_t pool_bytes(')
program+=function(ui,'bool make_pool_room(')
program+=function(renderer,'bool TextRenderer::ensureGlyphPool(')
program+=r'''
int main(){
    for(int i=0;i<12;++i){auto& b=banks[i];b.px=20+i;b.last_used=i+1;b.primary.init(65536);}
    auto& owner=banks[0];
    auto& r=owner.primary;
    r._style.allow_glyph_pool_growth=make_pool_room;r._style.glyph_pool_owner=&owner;
    r._glyph_pool_used=65536;std::memset(r._glyph_pool,0xa5,65536);
    assert(r.ensureGlyphPool(1));
    assert(owner.px && !banks[1].px && r._glyph_pool_capacity==131072);
    assert(pool_bytes()==UI_GLYPH_POOL_BUDGET);
    for(size_t i=0;i<65536;++i)assert(r._glyph_pool[i]==0xa5);
    r._glyph_pool_used=131072;
    assert(!r.ensureGlyphPool(1)); // Per-face limit.
    for(auto& b:banks)b.clear();
    r.init(8192);r._glyph_pool_used=8192;r._style.allow_glyph_pool_growth=nullptr;
    auto* original=r._glyph_pool;fail_alloc=true;
    assert(!r.ensureGlyphPool(1) && r._glyph_pool==original && r._glyph_pool_capacity==8192);
    fail_alloc=false;
    assert(r.ensureGlyphPool(1) && r._glyph_pool_capacity==16384);
    r._glyph_pool_used=16384;r._style.glyph_pool_initial_bytes=0;
    assert(!r.ensureGlyphPool(1)); // Existing reader fixed pool unchanged.
    r.end();
    assert(!pool_bytes());
    puts("UI font pools: growth/data preservation/total budget/owner safety/allocation failure/fixed reader PASS");
}
'''
Path(sys.argv[1]).write_text(program,encoding='utf-8')
