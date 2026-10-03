#include "map/MapCache.h"
#include "map/MapScale.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <sys/stat.h>
#ifdef _WIN32
#include <filesystem>
#ifndef S_ISREG
#define S_ISREG(mode) (((mode)&_S_IFMT)==_S_IFREG)
#endif
#else
#include <dirent.h>
#endif
#ifdef ESP_PLATFORM
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace {
constexpr size_t MAX_IMAGE = 4u*1024*1024;
struct Meta { uint32_t bytes = 0, crc = 0; uint64_t age = 0; };
struct Entry { std::string key; Meta meta; };
bool valid_key(const std::string &s) {
    if (s.size()<10 || s.size()>100 || (s.rfind("amap1_",0)!=0 && s.rfind("amap2_",0)!=0 && s.rfind("amap3_",0)!=0) || s.find("..")!=std::string::npos) return false;
    for (char c:s) if (!((c>='0'&&c<='9')||(c>='a'&&c<='z')||c=='_'||c=='.'||c=='-')) return false;
    return true;
}
std::string file(const std::string &dir,const std::string &key,const char *suffix) { return dir+"/"+key+suffix; }
void put(uint8_t *p,uint64_t n,int bytes) { for(int i=0;i<bytes;++i) { p[i]=uint8_t(n); n>>=8; } }
uint64_t get(const uint8_t *p,int bytes) { uint64_t n=0; for(int i=bytes-1;i>=0;--i) n=(n<<8)|p[i]; return n; }
bool read_meta(const std::string &path,Meta &m) {
    FILE *f=std::fopen(path.c_str(),"rb"); if(!f) return false;
    uint8_t b[32]; bool ok=std::fread(b,1,32,f)==32 && std::fgetc(f)==EOF;
    std::fclose(f);
    if(!ok||std::memcmp(b,"AMCACHE1",8)||get(b+24,8)) return false;
    m.bytes=uint32_t(get(b+8,4)); m.crc=uint32_t(get(b+12,4)); m.age=get(b+16,8);
    return m.bytes>0 && m.bytes<=MAX_IMAGE && m.age>0;
}
bool write_meta(const std::string &path,const Meta &m) {
    uint8_t b[32]={}; std::memcpy(b,"AMCACHE1",8);
    put(b+8,m.bytes,4); put(b+12,m.crc,4); put(b+16,m.age,8);
    const std::string temp=path+".tmp";
    FILE *f=std::fopen(temp.c_str(),"wb"); if(!f) return false;
    bool ok=std::fwrite(b,1,32,f)==32;
    if(std::fclose(f)) ok=false;
    // FAT rename does not replace an existing destination. Metadata is the commit marker.
    if(ok) { std::remove(path.c_str()); ok=std::rename(temp.c_str(),path.c_str())==0; }
    if(!ok) std::remove(temp.c_str());
    return ok;
}
std::vector<std::string> names(const std::string &dir) {
    std::vector<std::string> out;
#ifdef _WIN32
    std::error_code error;
    for (std::filesystem::directory_iterator i(dir,error),end; !error&&i!=end; i.increment(error))
        if(i->is_regular_file(error)) out.push_back(i->path().filename().string());
#else
    DIR *d=opendir(dir.c_str()); if(!d) return out;
    while(auto *e=readdir(d)) out.emplace_back(e->d_name);
    closedir(d);
#endif
    return out;
}
bool suffix_key(const std::string &name,const char *suffix,std::string &key) {
    const size_t n=std::strlen(suffix);
    if(name.size()<=n || name.compare(name.size()-n,n,suffix)) return false;
    key=name.substr(0,name.size()-n); return valid_key(key);
}
std::vector<Entry> scan(const std::string &dir) {
    std::vector<Entry> out;
    for(const auto &name:names(dir)) {
        std::string key; if(!suffix_key(name,".meta",key)) continue;
        Meta meta; struct stat st{};
        if(read_meta(file(dir,key,".meta"),meta)&&!stat(file(dir,key,".img").c_str(),&st)&&
           S_ISREG(st.st_mode)&&uint64_t(st.st_size)==meta.bytes) out.push_back({key,meta});
    }
    return out;
}
bool cleanup_uncommitted(const std::string &dir) {
    bool ok=true;
    for(const auto &name:names(dir)) {
        std::string key;
        bool orphan=suffix_key(name,".img.tmp",key)||suffix_key(name,".meta.tmp",key);
        if(!orphan && (suffix_key(name,".img",key)||suffix_key(name,".meta",key))) {
            Meta m; struct stat st{};
            orphan=!read_meta(file(dir,key,".meta"),m)||stat(file(dir,key,".img").c_str(),&st)||
                   !S_ISREG(st.st_mode)||uint64_t(st.st_size)!=m.bytes;
        }
        if(orphan) {
            const auto p=dir+"/"+name; struct stat st{};
            if(!stat(p.c_str(),&st)&&S_ISREG(st.st_mode)&&std::remove(p.c_str())) ok=false;
        }
    }
    return ok;
}
uint64_t next_age(const std::vector<Entry> &entries) {
    uint64_t age=0; for(const auto &e:entries) age=std::max(age,e.meta.age);
    return age==UINT64_MAX ? 0 : age+1;
}
bool image_crc(const char *path,Meta &m,const char *copy=nullptr) {
    FILE *in=std::fopen(path,"rb"); if(!in) return false;
    FILE *out=copy?std::fopen(copy,"wb"):nullptr;
    if(copy&&!out) { std::fclose(in); return false; }
    static const auto table=[] { struct T { uint32_t v[256]; } t{};
        for(uint32_t i=0;i<256;++i) { uint32_t c=i; for(int j=0;j<8;++j)c=(c>>1)^((c&1)?0xedb88320u:0); t.v[i]=c; } return t; }();
    uint8_t b[4096]; size_t bytes=0; uint32_t c=~0u; bool ok=true;
#ifdef ESP_PLATFORM
    int64_t yielded=esp_timer_get_time();
#endif
    while(size_t n=std::fread(b,1,sizeof(b),in)) {
        if(n>MAX_IMAGE-bytes) { ok=false; break; }
        bytes+=n;
        if(out&&std::fwrite(b,1,n,out)!=n) { ok=false; break; }
        for(size_t i=0;i<n;++i)c=table.v[(c^b[i])&255]^(c>>8);
#ifdef ESP_PLATFORM
        if(esp_timer_get_time()-yielded>=200000) { vTaskDelay(1); yielded=esp_timer_get_time(); }
#endif
    }
    if(std::ferror(in)) ok=false;
    std::fclose(in); if(out&&std::fclose(out)) ok=false;
    m.bytes=uint32_t(bytes); m.crc=~c;
    return ok&&bytes>0;
}
}

std::string MapCache::key(const AmapConfig &c,int w,int h,int scale) {
    if(c.offline||!std::isfinite(c.longitude)||!std::isfinite(c.latitude)||
       c.longitude<-180||c.longitude>180||c.latitude<-85.051129||c.latitude>85.051129||
       c.zoom<map_scale::online_min_zoom||c.zoom>map_scale::online_max_zoom||w<1||w>2048||h<1||h>2048||(scale!=1&&scale!=2)) return {};
    char b[101];
    std::snprintf(b,sizeof(b),"amap3_%.6f_%.6f_z%u_r%u_%dx%d_s%d",c.longitude,c.latitude,unsigned(c.zoom),unsigned(map_scale::request_zoom(c.zoom)),
                  std::max(1,std::min(1024,(w+1)/2)),std::max(1,std::min(1024,(h+1)/2)),scale);
    return b; // Exact normalized request parameters, never includes the API Key.
}

bool MapCache::lookup(const std::string &key,std::string *path) {
    if(!path||!valid_key(key)) return false;
    Meta saved,actual;
    const auto image=file(_directory,key,".img");
    if(!read_meta(file(_directory,key,".meta"),saved)||!image_crc(image.c_str(),actual)||
       actual.bytes!=saved.bytes||actual.crc!=saved.crc) { invalidate(key); return false; }
    saved.age=next_age(scan(_directory));
    if(saved.age) write_meta(file(_directory,key,".meta"),saved); // Failed touch still permits a verified hit.
    *path=image; return true;
}

bool MapCache::store(const std::string &key,const char *source,std::string *path) {
    if(!source||!path||!valid_key(key)||!_entries) return false;
    const auto image=file(_directory,key,".img"),temp=image+".tmp";
    if(image==source||temp==source) return false;
    if(!cleanup_uncommitted(_directory)) return false;
    Meta m;
    if(!image_crc(source,m,temp.c_str())) { std::remove(temp.c_str()); return false; }
    if(size_t(m.bytes)+32>_limit) { std::remove(temp.c_str()); return false; }
    auto entries=scan(_directory); m.age=next_age(entries);
    if(!m.age) { std::remove(temp.c_str()); return false; }
    size_t total=0,count=0;
    for(const auto &e:entries) if(e.key!=key) { total+=size_t(e.meta.bytes)+32; ++count; }
    std::sort(entries.begin(),entries.end(),[](const Entry &a,const Entry &b) { return a.meta.age<b.meta.age; });
    for(const auto &e:entries) {
        if(total+m.bytes+32<=_limit && count<_entries) break;
        if(e.key==key) continue;
        if(!invalidate(e.key)) { std::remove(temp.c_str()); return false; }
        total-=size_t(e.meta.bytes)+32; --count;
    }
    std::remove(file(_directory,key,".meta").c_str());
    std::remove(image.c_str());
    if(std::rename(temp.c_str(),image.c_str()) || !write_meta(file(_directory,key,".meta"),m)) {
        std::remove(temp.c_str()); invalidate(key); return false;
    }
    *path=image; return true;
}

bool MapCache::invalidate(const std::string &key) {
    if(!valid_key(key)) return false;
    bool ok=true;
    for(const char *suffix:{".meta",".img",".meta.tmp"}) {
        const auto p=file(_directory,key,suffix); struct stat st{};
        if(!stat(p.c_str(),&st) && ( !S_ISREG(st.st_mode) || std::remove(p.c_str()) )) ok=false;
    }
    return ok;
}

bool MapCache::clear() {
    bool ok=true;
    for(const auto &name:names(_directory)) {
        std::string key;
        if(!suffix_key(name,".meta",key)&&!suffix_key(name,".img",key)&&
           !suffix_key(name,".meta.tmp",key)&&!suffix_key(name,".img.tmp",key)) continue;
        const auto p=_directory+"/"+name; struct stat st{};
        if(!stat(p.c_str(),&st)&&S_ISREG(st.st_mode)&&std::remove(p.c_str())) ok=false;
    }
    return ok;
}

MapCache::Usage MapCache::usage() const {
    Usage u; for(const auto &e:scan(_directory)) { ++u.entries; u.bytes+=size_t(e.meta.bytes)+32; } return u;
}
