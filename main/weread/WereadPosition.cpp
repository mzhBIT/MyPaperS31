#include "weread/WereadPosition.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace weread::position {
namespace {
uint32_t next(std::string_view s,size_t& i) {
    unsigned char c=s[i++]; if(c<128) return c;
    unsigned n=(c&0xe0)==0xc0?1:(c&0xf0)==0xe0?2:(c&0xf8)==0xf0?3:0;
    uint32_t cp=c&((1u<<(6-n))-1);
    if(!n||i+n>s.size()) return 0xfffd;
    while(n--) { unsigned char b=s[i]; if((b&0xc0)!=0x80) return 0xfffd; cp=(cp<<6)|(b&63); ++i; }
    return cp;
}
void utf8(uint32_t cp,std::string& out) {
    if(cp<0x80) out+=char(cp);
    else if(cp<0x800) {out+=char(0xc0|(cp>>6));out+=char(0x80|(cp&63));}
    else if(cp<0x10000) {out+=char(0xe0|(cp>>12));out+=char(0x80|((cp>>6)&63));out+=char(0x80|(cp&63));}
    else {out+=char(0xf0|(cp>>18));out+=char(0x80|((cp>>12)&63));out+=char(0x80|((cp>>6)&63));out+=char(0x80|(cp&63));}
}
struct Text {
    std::string value;
    std::vector<uint64_t> origins;
    void append(uint32_t cp,uint64_t origin) {
        if(cp==0xa0||cp==0x3000||cp=='\n'||cp=='\r'||cp=='\t') cp=' ';
        if(cp==' '&&(value.empty()||value.back()==' ')) return;
        const size_t before=value.size(); utf8(cp,value);
        origins.insert(origins.end(),value.size()-before,origin);
    }
};
Text normalize(std::string_view s,bool html) {
    Text out; uint64_t utf16=0; std::string skipped;
    const bool explicit_coordinates=html&&s.find("data-wr-co")!=s.npos;
    bool in_body=false;
    struct Anchor { std::string tag; uint64_t offset, text_start; bool valid; };
    std::vector<Anchor> anchors;
    uint64_t visible_units=0;
    auto origin_at = [&]() -> uint64_t {
        if(!explicit_coordinates) return utf16;
        for(auto it=anchors.rbegin();it!=anchors.rend();++it)
            if(it->valid) return it->offset+visible_units-it->text_start;
        return UINT64_MAX;
    };
    for(size_t i=0;i<s.size();) {
        if(html&&s[i]=='<') {
            // Comments and '>' inside quoted attributes must not become text.
            size_t end=i+1;
            if(s.substr(i,4)=="<!--") {
                const auto close=s.find("-->",i+4);
                if(close==s.npos) break;
                end=close+2;
            } else {
                char quote=0;
                for(;end<s.size();++end) {
                    const char c=s[end];
                    if(quote) { if(c==quote) quote=0; }
                    else if(c=='\''||c=='"') quote=c;
                    else if(c=='>') break;
                }
                if(end==s.size()) break;
            }
            std::string tag(s.substr(i+1,end-i-1));
            std::transform(tag.begin(),tag.end(),tag.begin(),[](unsigned char c){return char(std::tolower(c));});
            size_t name_begin=(!tag.empty()&&tag[0]=='/')?1:0, name_end=name_begin;
            while(name_end<tag.size()&&(std::isalnum(static_cast<unsigned char>(tag[name_end]))||tag[name_end]=='-')) ++name_end;
            const auto name=tag.substr(name_begin,name_end-name_begin);
            if(name=="body") in_body=!name_begin;
            if(name_begin) {
                while(!anchors.empty()) { auto last=anchors.back().tag; anchors.pop_back(); if(last==name) break; }
            } else if(!name.empty()&&name!="br"&&name!="img"&&name!="hr"&&name!="meta"&&name!="link"&&tag.back()!='/') {
                Anchor anchor{name,0,visible_units,false};
                auto attr=tag.find("data-wr-co");
                if(attr!=tag.npos) {
                    attr+=10;
                    while(attr<tag.size()&&std::isspace(static_cast<unsigned char>(tag[attr]))) ++attr;
                    if(attr<tag.size()&&tag[attr++]=='=') {
                        while(attr<tag.size()&&std::isspace(static_cast<unsigned char>(tag[attr]))) ++attr;
                        if(attr<tag.size()&&(tag[attr]=='\''||tag[attr]=='"')) ++attr;
                        const size_t begin=attr;
                        while(attr<tag.size()&&std::isdigit(static_cast<unsigned char>(tag[attr]))) ++attr;
                        if(attr>begin&&(attr==tag.size()||tag[attr]=='\''||tag[attr]=='"'||std::isspace(static_cast<unsigned char>(tag[attr])))) {
                            anchor.offset=std::strtoull(tag.substr(begin,attr-begin).c_str(),nullptr,10);
                            anchor.valid=anchor.offset<=uint64_t(INT64_MAX)-s.size();
                        }
                    }
                }
                anchors.push_back(std::move(anchor));
            }
            if(tag.rfind("script",0)==0) skipped="script";
            else if(tag.rfind("style",0)==0) skipped="style";
            else if(tag.rfind("head",0)==0 && tag.rfind("header",0)!=0) skipped="head";
            else if(!skipped.empty()&&tag.rfind("/"+skipped,0)==0) skipped.clear();
            if(skipped.empty()&&(explicit_coordinates||in_body)&&(tag.rfind("/p",0)==0||name=="br"||tag.rfind("/div",0)==0)) out.append(' ',UINT64_MAX);
            while(i<=end) { auto cp=next(s,i); utf16+=cp>0xffff?2:1; }
            continue;
        }
        const uint64_t origin=origin_at();
        const bool visible=skipped.empty()&&(!html||explicit_coordinates||in_body);
        if(html&&visible&&s[i]=='&') {
            const auto end=s.find(';',i+1);
            if(end!=s.npos&&end-i<16) {
                const auto entity=s.substr(i+1,end-i-1); uint32_t cp=0;
                if(entity=="amp") cp='&'; else if(entity=="lt") cp='<'; else if(entity=="gt") cp='>';
                else if(entity=="quot") cp='"'; else if(entity=="apos") cp='\''; else if(entity=="nbsp") cp=' ';
                else if(!entity.empty()&&entity[0]=='#') {
                    std::string n(entity.substr(1)); char* tail=nullptr;
                    const bool hex=!n.empty()&&(n[0]=='x'||n[0]=='X');
                    const char* digits=n.c_str()+(hex?1:0);
                    auto value=std::strtoul(digits,&tail,hex?16:10);
                    if(tail!=digits&&*tail==0&&value<=0x10ffff&&!(value>=0xd800&&value<=0xdfff)) cp=uint32_t(value);
                }
                if(cp) { out.append(cp,origin); visible_units+=cp>0xffff?2:1; utf16+=end-i+1; i=end+1; continue; }
            }
        }
        auto cp=next(s,i); utf16+=cp>0xffff?2:1;
        if(visible) { out.append(cp,origin); visible_units+=cp>0xffff?2:1; }
    }
    while(!out.value.empty()&&out.value.back()==' ') {out.value.pop_back();out.origins.pop_back();}
    return out;
}
}
bool locate(std::string_view source,std::string_view excerpt,bool html,uint64_t* offset,const char** reason) {
    auto fail=[&](const char* why) { if(reason) *reason=why; return false; };
    if(reason) *reason="ok";
    if(!offset||source.empty()) return fail("empty-source");
    if(source.size()>512*1024) return fail("source-too-large");
    // Official utils parse() adds coordinates after locating each body in the
    // full original HTML. _41a0ac maps sourcePosition back to indexInFullHtml;
    // _314ca5 adds the raw text offset (JS string indices are UTF-16 units).
    // An unmarked fragment is not a full HTML payload; do not guess its basis.
    if(html&&source.find("data-wr-co")==source.npos&&source.find("<body")==source.npos)
        return fail("html-body-missing");
    auto needle=normalize(excerpt,false).value;
    // Match the same short excerpt sent by Web (20 UTF-16 units). Reader
    // summaries span multiple lines and may end at a byte-budget boundary.
    size_t end=0, units=0, kept=0;
    while(end<needle.size()) {
        const auto cp=next(needle,end); const size_t n=cp>0xffff?2:1;
        if(units+n>20) break;
        units+=n; kept=end;
    }
    needle.resize(kept);
    // Short excerpts are too weak to identify a precise reading position.
    if(needle.size()<12) return fail("excerpt-too-short");
    const auto haystack=normalize(source,html);
    const auto at=haystack.value.find(needle);
    if(at==std::string::npos) return fail("excerpt-not-found");
    if(haystack.value.find(needle,at+1)!=std::string::npos) return fail("excerpt-not-unique");
    if(haystack.origins[at]==UINT64_MAX) return fail("coordinate-missing");
    *offset=haystack.origins[at]; return true;
}
}
