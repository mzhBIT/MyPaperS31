#pragma once
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rss_feeds {
struct Feed {std::string title,url;};
constexpr size_t name_limit=255,url_limit=2048;
inline std::string trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r\n");
    if(first==std::string::npos)return {};
    return s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
inline bool valid_name(const std::string& name) {
    if(name.empty() || name.size()>name_limit || name[0]=='#')return false;
    for(unsigned char c:name)if(c<32 || c==127 || c=='|')return false;
    return true;
}
inline bool valid_url(const std::string& url) {
    if(url.size()>url_limit)return false;
    const size_t start=url.compare(0,7,"http://")==0 ? 7 : url.compare(0,8,"https://")==0 ? 8 : 0;
    if(!start || start==url.size())return false;
    for(unsigned char c:url)if(c<=32 || c==127)return false;
    const auto end=url.find_first_of("/?#",start);
    return (end==std::string::npos ? url.size() : end)>start;
}
enum class Load {Missing,Ok,Error};
inline bool duplicate_url(const std::vector<Feed>& feeds,const std::string& url,int ignore=-1) {
    for(size_t i=0;i<feeds.size();++i)if(int(i)!=ignore && feeds[i].url==url)return true;
    return false;
}
inline Load load(const std::string& path,std::vector<Feed>& out) {
    FILE* fp=std::fopen(path.c_str(),"rb");
    if(!fp) {
        if(errno!=ENOENT)return Load::Error;
        fp=std::fopen((path+".bak").c_str(),"rb");
        if(!fp)return errno==ENOENT ? Load::Missing : Load::Error;
    }
    std::vector<Feed> parsed;
    char line[4096];
    bool first=true,ok=true;
    while(std::fgets(line,sizeof(line),fp)) {
        if(!std::strchr(line,'\n') && !std::feof(fp)) {
            int c;while((c=std::fgetc(fp))!='\n' && c!=EOF){}
            continue; // Never interpret a partial overlong line as another feed.
        }
        std::string text=trim(line);
        if(first && text.size()>=3 && static_cast<unsigned char>(text[0])==0xEF &&
            static_cast<unsigned char>(text[1])==0xBB && static_cast<unsigned char>(text[2])==0xBF)
            text=trim(text.substr(3));
        first=false;
        if(text.empty() || text[0]=='#')continue;
        const auto separator=text.find('|');
        Feed feed=separator==std::string::npos ? Feed{text,text} :
            Feed{trim(text.substr(0,separator)),trim(text.substr(separator+1))};
        if(feed.title.empty())feed.title=feed.url;
        if(!valid_url(feed.url))continue;
        parsed.push_back(std::move(feed));
    }
    ok=!std::ferror(fp);
    std::fclose(fp);
    if(!ok)return Load::Error;
    out.swap(parsed);
    return Load::Ok; // An intentionally empty file stays empty across reboot.
}
inline bool save(const std::string& path,const std::vector<Feed>& feeds) {
    for(const auto& feed:feeds)
        if(!valid_name(feed.title) || !valid_url(feed.url))return false;
    const auto temp=path+".tmp",backup=path+".bak";
    FILE* fp=std::fopen(temp.c_str(),"wb");
    if(!fp)return false;
    bool ok=std::fputs("# MyPaper RSS feeds: name|URL\n",fp)>=0;
    for(const auto& feed:feeds) {
        const auto line=feed.title+"|"+feed.url+"\n";
        if(ok)ok=std::fwrite(line.data(),1,line.size(),fp)==line.size();
    }
    if(ok)ok=std::fflush(fp)==0;
    if(std::fclose(fp)!=0)ok=false;
    if(!ok){std::remove(temp.c_str());return false;}
    FILE* previous=std::fopen(path.c_str(),"rb");
    const bool had_previous=previous!=nullptr;
    if(had_previous) {
        std::fclose(previous);
        std::remove(backup.c_str());
        if(std::rename(path.c_str(),backup.c_str())!=0){std::remove(temp.c_str());return false;}
    } else if(errno!=ENOENT){std::remove(temp.c_str());return false;}
    if(std::rename(temp.c_str(),path.c_str())!=0) {
        if(had_previous)std::rename(backup.c_str(),path.c_str());
        std::remove(temp.c_str());return false;
    }
    std::remove(backup.c_str());
    return true;
}
} // namespace rss_feeds
