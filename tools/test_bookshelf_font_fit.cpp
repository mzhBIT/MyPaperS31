#include "ui/BookshelfFontFit.h"
#include <cassert>
static_assert(bookshelf_font_fit::estimate(33,85,100)==27);
static_assert(bookshelf_font_fit::estimate(10,1,100)==8);
int main() {
    bookshelf_font_fit::Cache cache;
    int calls=0;
    auto measure=[&](const std::string&,int px) { ++calls; return px*4; };
    const int fitted=cache.fit("button",33,100,1,measure);
    assert(fitted>=8 && fitted*4<=100 && calls<=3);
    const int original_calls=calls;
    for(int i=0;i<100;++i) assert(cache.fit("button",33,100,1,measure)==fitted);
    assert(calls==original_calls);
    cache.fit("button",33,100,2,measure); // Font revision invalidates.
    assert(calls>original_calls);
    int before=calls;
    cache.fit("translated",33,100,2,measure);
    assert(calls>before);
    before=calls;
    cache.fit("button",33,120,2,measure);
    assert(calls>before);
    calls=0;
    auto nonlinear=[&](const std::string&,int px) { ++calls; return px>8?9999:8; };
    assert(cache.fit("pathological",48,20,3,nonlinear)==8);
    assert(calls<=3);
    // Cyclic cache remains bounded and keeps the newest fit.
    for(int i=0;i<100;++i) cache.fit(std::to_string(i),30,100,1,measure);
    before=calls;
    cache.fit("99",30,100,1,measure);
    assert(calls==before);
}
