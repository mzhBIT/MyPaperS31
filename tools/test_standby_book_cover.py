"""Generate a native test of production standby artwork selection and cleanup.

Usage: python tools/test_standby_book_cover.py output.cpp
Compile as C++17 with main on the include path; no SDK or hardware is required.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (root / path).read_text(encoding='utf-8')
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


program = r'''
#include "activities/Activity.h"
#include "ui/ReaderProfile.h"
#include <cassert>
#include <cstdio>
#include <string>
const char *shared_path="shared.jpg", *rss_path="rss.jpg";
const char *reader_book_cover_path(ReaderProfile p=ReaderProfile::Shared) {
    return p==ReaderProfile::Rss ? rss_path : shared_path;
}
struct TxtReader : Activity {
    void on_create() override {}
'''
signature = 'const char *standby_book_cover_path() const override'
program += function('main/activities/ReaderActivity.cpp', signature)
program += r'''
};
struct ReflowReader : Activity {
    bool _ok=true;
    void on_create() override {}
'''
program += function('main/activities/EpubReaderActivity.cpp', signature)
program += r'''
};
struct RssReader : Activity {
    enum class View { Feeds, Items, Article };
    View _view=View::Article;
    bool bing=false;
    bool bing_wallpaper_mode() const {return bing;}
    void on_create() override {}
'''
program += function('main/activities/RssReaderActivity.cpp', signature)
program += r'''
};
namespace lgfx {struct LGFXBase {};}
struct {lgfx::LGFXBase Display;} M5;
struct ImageCache {
    std::string background_path, background_ext;
    int background_width=0, background_height=0;
    bool background_progressive=false;
    bool probe_ok=true, cache_ok=true;
    bool background_cache=false;
    int disk_loads=0, compositions=0;
    void release_cache() {background_cache=false;}
    std::string image_extension(const std::string &p) {
        return p.empty() || p=="unsupported" ? "" : p.find(".png")!=std::string::npos ? ".png" : ".jpg";
    }
    bool probe_image(const std::string&,const std::string&,int*w,int*h,bool*) {
        *w=640;*h=480;return probe_ok;
    }
    bool ensure_cache(int,int) {++disk_loads;background_cache=cache_ok;return cache_ok;}
    bool generate_cache(const std::string&,const std::string&,int,int,int,int,lgfx::LGFXBase *page) {
        assert(page);++compositions;background_cache=cache_ok;return cache_ok;
    }
    bool reader_background_draw(lgfx::LGFXBase&,int,int) {return background_cache;}
} book_cover_settings;
void reader_book_cover_release();
bool reader_book_cover_prepare(const char*,int,int,lgfx::LGFXBase* = nullptr);
'''
for name in ('bool reader_book_cover_prepare(', 'bool reader_book_cover_draw(',
             'void reader_book_cover_release('):
    program += function('main/ui/reader_background.cpp', name) + '\n'
program += r'''
bool system_enabled=true, system_prepares=true;
int system_attempts=0;
bool standby_wallpaper_enabled(){return system_enabled;}
bool standby_wallpaper_prepare(int,int){++system_attempts;return system_prepares;}
enum class StandbyImage { None, SystemWallpaper, BookCover };
'''
program += function('main/boards/MyPaperS31/StandbyS31.cpp', 'StandbyImage prepare_standby_image(')
program += r'''
int main() {
    TxtReader txt;ReflowReader reflow;RssReader rss;
    lgfx::LGFXBase gfx;
    auto prepare=[&](const Activity &a,bool shutdown=false) {
        return prepare_standby_image(a,shutdown,684,1216);
    };
    auto finish=[&] {
        reader_book_cover_release();
        assert(!book_cover_settings.background_cache && book_cover_settings.background_path.empty());
    };
    for(const Activity *page : {static_cast<Activity*>(&txt),static_cast<Activity*>(&reflow)}) {
        assert(prepare(*page)==StandbyImage::BookCover && system_attempts==0);
        assert(book_cover_settings.background_path==shared_path);
        assert(reader_book_cover_draw(gfx,684,1216));finish();
    }
    assert(prepare(rss)==StandbyImage::BookCover);
    assert(book_cover_settings.background_path==rss_path);finish();
    for(auto view : {RssReader::View::Feeds,RssReader::View::Items}) {
        rss._view=view;assert(prepare(rss)==StandbyImage::SystemWallpaper);finish();
    }
    rss._view=RssReader::View::Article;rss.bing=true;
    assert(prepare(rss)==StandbyImage::SystemWallpaper);finish();
    reflow._ok=false;assert(prepare(reflow)==StandbyImage::SystemWallpaper);finish();
    assert(prepare(txt,true)==StandbyImage::SystemWallpaper);finish();
    shared_path="";assert(prepare(txt)==StandbyImage::SystemWallpaper);finish();
    shared_path="missing.jpg";book_cover_settings.probe_ok=false;
    assert(prepare(txt)==StandbyImage::SystemWallpaper);finish();
    book_cover_settings.probe_ok=true;book_cover_settings.cache_ok=false;
    assert(prepare(txt)==StandbyImage::SystemWallpaper);finish();
    system_enabled=false;
    assert(prepare(txt)==StandbyImage::None);finish();
    system_enabled=true;system_prepares=false;
    assert(prepare(txt)==StandbyImage::None);finish();
    book_cover_settings.cache_ok=true;
    assert(prepare(txt)==StandbyImage::BookCover);finish();
    shared_path="transparent.png";
    int disk_loads=book_cover_settings.disk_loads;
    for(int i=0;i<2;++i) {
        assert(prepare(txt)==StandbyImage::BookCover);finish();
    }
    assert(book_cover_settings.disk_loads==disk_loads && book_cover_settings.compositions==2);
    assert(!reader_book_cover_prepare("transparent.png",684,1216));finish();
    assert(!reader_book_cover_prepare("unsupported",684,1216));finish();
    assert(!reader_book_cover_prepare("valid.jpg",0,1216));finish();
    assert(!reader_book_cover_draw(gfx,684,1216));
    puts("Standby artwork PASS: foreground/profile selection, fallback, shutdown, invalid images and buffer cleanup.");
}
'''
out = Path(sys.argv[1])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(program, encoding='utf-8')
