#include "weread/WereadStore.h"
#include "storage/app_paths.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <mutex>

namespace weread::store {
namespace {
std::mutex queue_mutex;
constexpr uint32_t magic = 0x36505257; // WRP6: owned queue + durable report receipts
constexpr uint32_t max_records = 10000, max_string = 1024*1024;
template<class T> bool put(FILE* f, T value) { return fwrite(&value,sizeof(value),1,f)==1; }
template<class T> bool get(FILE* f, T* value) { return fread(value,sizeof(*value),1,f)==1; }
bool put_string(FILE* f,const std::string& s) {
    return s.size()<=max_string && put(f,uint32_t(s.size())) &&
        (s.empty() || fwrite(s.data(),1,s.size(),f)==s.size());
}
bool get_string(FILE* f,std::string* s) {
    uint32_t n=0; if(!get(f,&n)||n>max_string) return false;
    s->resize(n); return !n || fread(s->data(),1,n,f)==n;
}
std::string path(const std::string& account) {
    std::string p=std::string(app_paths::weread_dir())+"/progress_";
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:account) { p+=hex[c>>4]; p+=hex[c&15]; }
    return p+".bin";
}
enum class Read { Missing, Invalid, Valid };
Read read_file(const std::string& p, std::vector<PendingProgress>* records, bool unique_books=true) {
    FILE* f=fopen(p.c_str(),"rb");
    if(!f) return errno==ENOENT?Read::Missing:Read::Invalid;
    uint32_t m=0,n=0; bool ok=get(f,&m)&&m==magic&&get(f,&n)&&n<=max_records;
    std::vector<PendingProgress> result;
    for(uint32_t i=0;ok&&i<n;++i) {
        PendingProgress v; uint32_t index=0,percent=0,verified=0,state=0,rpercent=0;
        ok=get_string(f,&v.account_vid)&&get_string(f,&v.book_id)&&get_string(f,&v.chapter.uid)&&
           get_string(f,&v.chapter.title)&&get(f,&index)&&index<=INT32_MAX&&get(f,&percent)&&(percent<=100||percent==UINT32_MAX)&&
           get(f,&v.active_seconds)&&get(f,&v.chapter_offset)&&get_string(f,&v.summary)&&
           get(f,&v.recorded_at)&&get(f,&v.revision)&&v.revision&&get(f,&verified)&&verified<=1&&
           get(f,&state)&&state<=2&&get(f,&v.receipt.seconds)&&get(f,&v.receipt.revision)&&
           get(f,&v.receipt.reading_before)&&get(f,&v.receipt.submitted_at)&&
           get_string(f,&v.receipt.chapter_uid)&&get(f,&v.receipt.chapter_offset)&&get(f,&rpercent)&&rpercent<=100;
        v.chapter.index=int(index); v.percent=percent==UINT32_MAX?-1:int(percent); v.position_verified=verified;
        v.receipt.state=ReportReceipt::State(state); v.receipt.percent=int(rpercent);
        if(v.book_id.empty()||v.chapter.uid.empty()||v.account_vid.empty()||v.account_vid.size()>96||
           v.receipt.seconds>v.active_seconds||v.receipt.revision>v.revision) ok=false;
        if(unique_books&&std::any_of(result.begin(),result.end(),[&](const auto& p){return p.book_id==v.book_id;})) ok=false;
        if(ok) result.push_back(std::move(v));
    }
    ok=ok&&fgetc(f)==EOF&&!ferror(f); fclose(f);
    if(!ok) return Read::Invalid;
    *records=std::move(result); return Read::Valid;
}
bool read(const std::string& account,std::vector<PendingProgress>* records) {
    const auto p=path(account); const auto a=read_file(p,records);
    if(a==Read::Valid) {
        return std::all_of(records->begin(),records->end(),[&](const auto& v){return v.account_vid==account;});
    }
    const auto b=read_file(p+".bak",records);
    if(b==Read::Valid) {
        // A backup may predate an in-flight report. Recover its data without
        // ever replaying seconds whose submission journal was lost.
        for(auto& v:*records) if(v.receipt.state==ReportReceipt::State::Ready) {
            v.receipt.state=ReportReceipt::State::InFlight;
            v.receipt.revision=v.revision; v.receipt.seconds=v.active_seconds;
            v.receipt.reading_before=-1; v.receipt.chapter_uid=v.chapter.uid;
            v.receipt.chapter_offset=v.chapter_offset;
        }
        return std::all_of(records->begin(),records->end(),[&](const auto& v){return v.account_vid==account;});
    }
    if(a!=Read::Missing||b!=Read::Missing) return false;
    records->clear(); return true;
}
bool write_path(const std::string& p,const std::vector<PendingProgress>& records,bool unique_books=true) {
    if(records.size()>max_records) return false;
    ensure_directories();
    const auto tmp=p+".tmp",bak=p+".bak";
    FILE* f=fopen(tmp.c_str(),"wb"); if(!f) return false;
    bool ok=put(f,magic)&&put(f,uint32_t(records.size()));
    for(const auto& v:records) {
        const auto& r=v.receipt;
        ok=ok&&put_string(f,v.account_vid)&&put_string(f,v.book_id)&&put_string(f,v.chapter.uid)&&
           put_string(f,v.chapter.title)&&put(f,uint32_t(v.chapter.index))&&put(f,uint32_t(v.percent))&&
           put(f,v.active_seconds)&&put(f,v.chapter_offset)&&put_string(f,v.summary)&&
           put(f,v.recorded_at)&&put(f,v.revision)&&put(f,uint32_t(v.position_verified))&&
           put(f,uint32_t(r.state))&&put(f,r.seconds)&&put(f,r.revision)&&put(f,r.reading_before)&&
           put(f,r.submitted_at)&&put_string(f,r.chapter_uid)&&put(f,r.chapter_offset)&&put(f,uint32_t(r.percent));
    }
    ok=fflush(f)==0&&ok; ok=fclose(f)==0&&ok;
    if(!ok) { remove(tmp.c_str()); return false; }
    std::vector<PendingProgress> previous;
    const auto primary=read_file(p,&previous,unique_books);
    if(primary==Read::Valid) {
        if(remove(bak.c_str())!=0&&errno!=ENOENT) return false;
        if(rename(p.c_str(),bak.c_str())!=0) return false;
    } else if(primary==Read::Invalid && remove(p.c_str())!=0) return false;
    return rename(tmp.c_str(),p.c_str())==0;
}
bool write(const std::string& account,const std::vector<PendingProgress>& records) {
    return write_path(path(account),records);
}
}
bool save_pending_progress(const PendingProgress& incoming) {
    if(incoming.account_vid.empty()||incoming.account_vid.size()>96||incoming.book_id.empty()||
       incoming.chapter.uid.empty()||incoming.chapter.index<0||incoming.percent< -1||incoming.percent>100) return false;
    std::lock_guard<std::mutex> lock(queue_mutex);
    std::vector<PendingProgress> records; if(!read(incoming.account_vid,&records)) return false;
    auto it=std::find_if(records.begin(),records.end(),[&](const auto& p){return p.book_id==incoming.book_id;});
    PendingProgress merged=incoming; merged.receipt={}; merged.revision=1;
    if(it!=records.end()) {
        const uint64_t seconds=uint64_t(it->active_seconds)+incoming.active_seconds;
        if(seconds>UINT32_MAX||it->revision==UINT64_MAX) return false;
        merged.active_seconds=uint32_t(seconds); merged.revision=it->revision+1;
        merged.receipt=it->receipt; *it=std::move(merged);
    } else records.push_back(std::move(merged));
    return write(incoming.account_vid,records);
}
bool list_pending_progress(const std::string& account,std::vector<PendingProgress>* records) {
    if(!records||account.empty()||account.size()>96) return false;
    std::lock_guard<std::mutex> lock(queue_mutex); return read(account,records);
}
bool load_pending_progress(const std::string& account,const std::string& book,PendingProgress* result) {
    if(!result) return false;
    std::vector<PendingProgress> records; if(!list_pending_progress(account,&records)) return false;
    const auto it=std::find_if(records.begin(),records.end(),[&](const auto& p){return p.book_id==book;});
    if(it==records.end()) return false;
    *result=*it; return true;
}
bool load_pending_progress(PendingProgress* result) {
    if(!result) return false;
    Session session; if(!load_session(&session)||session.vid.empty()) return false;
    std::vector<PendingProgress> records;
    if(!list_pending_progress(session.vid,&records)) return false;
    if(records.empty()) return false;
    *result=*std::max_element(records.begin(),records.end(),[](const auto& a,const auto& b){return a.recorded_at<b.recorded_at;});
    return true;
}
bool checkpoint_progress(const PendingProgress& snapshot,const ReportReceipt& receipt) {
    if(snapshot.account_vid.empty()||snapshot.account_vid.size()>96||!snapshot.revision||
       (receipt.state!=ReportReceipt::State::Ready&&!receipt.revision)) return false;
    std::lock_guard<std::mutex> lock(queue_mutex);
    std::vector<PendingProgress> records; if(!read(snapshot.account_vid,&records)) return false;
    auto it=std::find_if(records.begin(),records.end(),[&](const auto& p){return p.book_id==snapshot.book_id;});
    if(it==records.end()||it->revision<snapshot.revision||receipt.revision>it->revision||receipt.seconds>it->active_seconds) return false;
    if(it->receipt.state!=ReportReceipt::State::Ready&&it->receipt.revision!=receipt.revision) return false;
    it->receipt=receipt; return write(snapshot.account_vid,records);
}
bool acknowledge_progress(const PendingProgress& snapshot) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    std::vector<PendingProgress> records; if(!read(snapshot.account_vid,&records)) return false;
    auto it=std::find_if(records.begin(),records.end(),[&](const auto& p){return p.book_id==snapshot.book_id;});
    const auto& r=snapshot.receipt;
    if(it==records.end()||!r.revision||it->receipt.revision!=r.revision||it->active_seconds<r.seconds) return false;
    // A live report can cover only part of a checkpoint; the rest may be
    // pre-session/offline time and must survive even at the same revision.
    if(it->revision==r.revision&&it->active_seconds==r.seconds) records.erase(it);
    else { it->active_seconds-=r.seconds; it->receipt={}; }
    return write(snapshot.account_vid,records);
}
bool hold_pending_progress(const PendingProgress& snapshot) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    std::vector<PendingProgress> records;
    if(!read(snapshot.account_vid,&records)) return false;
    auto it=std::find_if(records.begin(),records.end(),[&](const auto& p){return p.book_id==snapshot.book_id;});
    if(it==records.end()||it->revision!=snapshot.revision||
       it->receipt.revision!=snapshot.receipt.revision||it->receipt.state!=snapshot.receipt.state) return false;
    const auto held_path=path(snapshot.account_vid)+".held";
    std::vector<PendingProgress> held;
    auto a=read_file(held_path,&held,false);
    if(a!=Read::Valid) {
        const auto b=read_file(held_path+".bak",&held,false);
        if(b!=Read::Valid&&(a!=Read::Missing||b!=Read::Missing)) return false;
    }
    if(!std::all_of(held.begin(),held.end(),[&](const auto &p){return p.account_vid==snapshot.account_vid;})) return false;
    // Archive first, then detach. A power loss between the writes may leave a
    // duplicate *held* copy, but can never make historical seconds replayable.
    held.push_back(*it);
    if(!write_path(held_path,held,false)) return false;
    records.erase(it);
    return write(snapshot.account_vid,records);
}
}
