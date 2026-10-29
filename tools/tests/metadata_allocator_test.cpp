#include "memory/MetadataAllocator.h"
#include <cassert>
#include <cstdio>
#include <string>

struct Row {metadata_memory::String title,href;int index=0;};
int main(){
    metadata_memory::Vector<Row> rows;
    for(int i=0;i<2000;++i)
        rows.push_back({"Chapter title longer than the small-string storage "+std::to_string(i),
                        "text/chapter/long-resource-path-"+std::to_string(i)+".html",i});
    metadata_memory::Map<int> index;
    for(const auto& r:rows)index[r.href]=r.index;
    assert(index.find(std::string(rows[1999].href))->second==1999);
    assert(rows[1].title==std::string("Chapter title longer than the small-string storage 1"));
    assert(std::string(rows[1].href.substr(0,4))=="text");
    auto* old=rows.data();
    auto moved=std::move(rows);
    assert(moved.data()==old && moved.size()==2000);
    metadata_memory::release(moved);metadata_memory::release(index);
    assert(moved.capacity()==0 && index.empty());
    metadata_memory::Budget budget;
    assert(budget.add(0,sizeof(Row),{100,100}));
    assert(!budget.add(metadata_memory::Budget::max_entries,sizeof(Row),{}));
    budget={};assert(!budget.add(0,sizeof(Row),{16385}));
    budget={};int count=0;
    while(budget.add(count,sizeof(Row),{256}))++count;
    assert(count>1000 && budget.exceeded && budget.used<=metadata_memory::Budget::max_bytes);
    assert(!metadata_memory::Budget::reserve_allowed(100000,sizeof(Row)));
    std::puts("Metadata: strings/index/vector conversions, move without copy, release, bounds PASS");
}
