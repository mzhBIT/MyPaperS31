#include "rss/FeedStore.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static void write_file(const std::string& path,const std::string& text) {
    auto* fp=std::fopen(path.c_str(),"wb");assert(fp);
    assert(std::fwrite(text.data(),1,text.size(),fp)==text.size());assert(!std::fclose(fp));
}
int main() {
    using namespace rss_feeds;
    const std::string path=".codex-build-cache/rss-feeds-test.txt";
    std::remove(path.c_str());std::remove((path+".bak").c_str());std::remove((path+".tmp").c_str());
    std::vector<Feed> feeds;
    assert(load(path,feeds)==Load::Missing);
    assert(valid_name("新闻订阅") && !valid_name("") && !valid_name("a|b") && !valid_name("a\nb") && !valid_name("#comment"));
    assert(valid_url("https://example.com/rss?a=1&b=2") && valid_url("http://127.0.0.1:8000/rss"));
    assert(!valid_url("https://") && !valid_url("https://?q=1") && !valid_url("file:///sdcard/book") && !valid_url("https://example.com/line\nbreak"));
    feeds={{"中文新闻","https://example.com/a"},{"World","http://example.com/b"}};
    assert(save(path,feeds));
    std::vector<Feed> readback;assert(load(path,readback)==Load::Ok);
    assert(readback.size()==2 && readback[0].title==feeds[0].title && readback[1].url==feeds[1].url);
    assert(duplicate_url(readback,"https://example.com/a"));
    assert(!duplicate_url(readback,"https://example.com/a",0));
    readback[0].title="新的名称";readback[0].url="https://example.com/new";
    assert(save(path,readback) && load(path,feeds)==Load::Ok && feeds[0].title=="新的名称");
    auto invalid=feeds;invalid[0].url="invalid";
    assert(!save(path,invalid));
    assert(load(path,readback)==Load::Ok && readback[0].url=="https://example.com/new");
    // Failed destination writes cannot touch the existing config.
    assert(!save(".codex-build-cache/no-such-directory/rss.txt",feeds));
    assert(load(path,readback)==Load::Ok && readback.size()==2);
    assert(save(path,{}));assert(load(path,readback)==Load::Ok && readback.empty());
    assert(save(path,feeds));
    assert(!std::rename(path.c_str(),(path+".bak").c_str()));
    assert(load(path,readback)==Load::Ok && readback.size()==2); // Recover after interrupted replacement.
    assert(save(path,readback));
    std::string legacy="\xEF\xBB\xBF# comment\r\nName|https://example.com/rss\r\nhttps://example.com/legacy\n";
    legacy+=std::string(5000,'x')+"\nOther|http://example.com/other\n";
    write_file(path,legacy);
    assert(load(path,readback)==Load::Ok && readback.size()==3);
    assert(readback[1].title=="https://example.com/legacy" && readback[2].title=="Other");
    std::remove(path.c_str());std::remove((path+".bak").c_str());std::remove((path+".tmp").c_str());
    std::puts("RSS feed store: add/edit/delete/empty/UTF-8/legacy/BOM/recovery/failed writes/validation PASS");
}
