#include "weread/WereadReport.h"
#include "weread/WereadStore.h"
#include "weread/WereadPosition.h"
#include "weread/WereadReadingTime.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
std::string root;
std::string owner="100";
}
namespace app_paths { const char* weread_dir() { return root.c_str(); } }
namespace weread {
bool load_session(Session* session) { session->vid=owner; return true; }
namespace store {
bool ensure_directories() { std::filesystem::create_directories(root); return true; }
bool load_legacy_pending_progress(PendingProgress*) { return false; }
}
}
int main(int argc,char** argv) {
    assert(argc==2); root=argv[1];
    assert(std::filesystem::is_directory(root));
    using namespace weread;
    // Only actual activity after entry acknowledgement is live-accountable.
    assert(reading_time::live_interval(100,200,0)==0);
    assert(reading_time::live_interval(100,200,150)==50);
    assert(reading_time::live_interval(100,200,250)==0);
    assert(reading_time::live_interval(200,300,150)==100);
    auto slice=reading_time::due(65'900'000,60'300'000,0,0,true);
    assert(slice.total==65&&slice.live==60); // 5 pre-session seconds held
    slice=reading_time::due(126'100'000,120'600'000,65,60,true);
    assert(slice.total==61&&slice.live==60); // no replay of first interval
    slice=reading_time::due(126'100'000,120'600'000,65,60,false);
    assert(slice.total==61&&slice.live==0); // context lost: no offline replay
    slice=reading_time::due(187'100'000,181'600'000,126,120,true);
    assert(slice.total==61&&slice.live==61); // re-entry excludes old live counter
    slice=reading_time::due(60'100'000,59'900'000,60,59,true);
    assert(slice.total==0&&slice.live==0); // sub-second rounding cannot invent time
    assert(report::accepted(R"({"succ":1})"));
    assert(report::accepted(R"({"data":{"succ":true}})"));
    assert(report::accepted(R"({"synckey":0})"));
    assert(!report::accepted(R"({"errCode":-2054,"succ":1})"));
    assert(!report::accepted(R"({"data":{"errCode":-2012},"succ":1})"));
    assert(!report::accepted("{}"));
    assert(!report::accepted("<html>login</html>"));
    std::vector<Chapter> chapters(3);
    chapters[0].word_count=100; chapters[1].word_count=800; chapters[2].word_count=100;
    assert(report::weighted_percent(chapters,1,1,2)==50);
    assert(report::weighted_percent(chapters,2,0,10)==90);
    assert(report::weighted_percent({},0,0,1)==-1);
    chapters[0].word_count=-1;
    assert(report::weighted_percent(chapters,0,0,1)==-1);
    ReportReceipt r; r.state=ReportReceipt::State::Accepted; r.seconds=30;
    r.reading_before=100; r.chapter_uid="3"; r.chapter_offset=18;
    RemoteProgress remote; remote.position_known=true; remote.chapter_uid="3"; remote.chapter_offset=18;
    remote.reading_seconds=129; assert(!report::confirmed(r,remote));
    remote.reading_seconds=130; assert(report::confirmed(r,remote));
    remote.chapter_offset=19; assert(!report::confirmed(r,remote));
    remote.chapter_offset=18; r.reading_before=-1; assert(!report::confirmed(r,remote));
    uint64_t offset=999;
    assert(position::locate("<html><body><p data-wr-co='500'>unique sentence here &amp; more</p></body></html>",
                            "unique sentence here & more",true,&offset)&&offset==500);
    assert(position::locate(u8"<p data-wr-co='10'>😀这是原始章节的一段正文</p>",u8"这是原始章节的一段正文",true,&offset)&&offset==12);
    assert(!position::locate("<p>unique sentence here</p>","unique sentence here",true,&offset));
    const char *reason=nullptr;
    assert(position::locate("<html><head><title>title</title></head><body><p>unique sentence here</p></body></html>",
                            "unique sentence here",true,&offset,&reason)&&offset==48);
    assert(std::string(reason)=="ok");
    // Raw UTF-16 offsets include headers, original tag attributes and entities.
    assert(position::locate(u8"<head>😀</head><body><p title='a>b'>甲&amp;乙这是原始章节的一段正文</p></body>",
                            u8"乙这是原始章节的一段正文",true,&offset)&&offset==42);
    assert(position::locate("<body><p>skip <!-- unique hidden phrase -->unique visible phrase</p></body>",
                            "unique visible phrase",true,&offset)&&offset==43);
    assert(position::locate("<body>first body only</body>separator<body>unique second body phrase</body>",
                            "unique second body phrase",true,&offset)&&offset==43);
    assert(!position::locate("<body>repeat sentence here; repeat sentence here</body>",
                             "repeat sentence here",true,&offset,&reason));
    assert(std::string(reason)=="excerpt-not-unique");
    assert(!position::locate("<body>other visible phrase</body>","missing sentence here",true,&offset,&reason));
    assert(std::string(reason)=="excerpt-not-found");
    assert(position::locate("first\nunique sentence here\nlast","unique sentence here",false,&offset)&&offset==6);
    assert(!position::locate("<p>repeat sentence here</p><p>repeat sentence here</p>","repeat sentence here",true,&offset));
    assert(!position::locate("missing","short",false,&offset));
    assert(position::locate("a unique first twenty characters and a long suffix",
                            "a unique first twenty characters and a truncated suffix",false,&offset)&&offset==0);
    assert(!position::locate("<head>unique hidden phrase</head><p>other text</p>","unique hidden phrase",true,&offset));
    using store::PendingProgress;
    PendingProgress a; a.account_vid="100"; a.book_id="A"; a.chapter.uid="3"; a.chapter.index=1;
    a.percent=10; a.active_seconds=30; a.recorded_at=1704067300; a.summary="unique sentence here";
    assert(store::save_pending_progress(a));
    auto b=a; b.book_id="B"; b.active_seconds=15; b.percent=-1;
    assert(store::save_pending_progress(b));
    auto other=a; other.account_vid="200"; other.active_seconds=99;
    assert(store::save_pending_progress(other));
    std::vector<PendingProgress> records;
    assert(store::list_pending_progress("100",&records)&&records.size()==2);
    PendingProgress snapshot;
    assert(store::load_pending_progress("100","A",&snapshot)&&snapshot.active_seconds==30);
    snapshot.receipt.state=ReportReceipt::State::InFlight;
    snapshot.receipt.revision=snapshot.revision; snapshot.receipt.seconds=30;
    snapshot.receipt.chapter_uid="3"; snapshot.receipt.reading_before=100;
    assert(store::checkpoint_progress(snapshot,snapshot.receipt));
    a.active_seconds=5; a.percent=11;
    assert(store::save_pending_progress(a));
    PendingProgress newer;
    assert(store::load_pending_progress("100","A",&newer)&&newer.active_seconds==35);
    assert(newer.receipt.state==ReportReceipt::State::InFlight);
    snapshot.receipt.state=ReportReceipt::State::Accepted;
    assert(store::checkpoint_progress(snapshot,snapshot.receipt));
    assert(store::acknowledge_progress(snapshot));
    assert(store::load_pending_progress("100","A",&newer)&&newer.active_seconds==5&&newer.percent==11);
    assert(newer.receipt.state==ReportReceipt::State::Ready);
    assert(!store::acknowledge_progress(snapshot)); // Stale ack cannot delete new data.
    assert(store::load_pending_progress("200","A",&newer)&&newer.active_seconds==99);
    assert(store::load_pending_progress("100","B",&b)&&b.percent==-1);
    auto uncertain=b; uncertain.receipt.state=ReportReceipt::State::InFlight;
    uncertain.receipt.revision=b.revision; uncertain.receipt.seconds=b.active_seconds;
    assert(store::checkpoint_progress(uncertain,uncertain.receipt));
    assert(store::load_pending_progress("100","B",&b)&&b.receipt.state==ReportReceipt::State::InFlight);
    // An uncertain receipt (the real-device 199s case) does not block new time.
    auto held=a; held.book_id="held-book"; held.active_seconds=199;
    assert(store::save_pending_progress(held));
    assert(store::load_pending_progress("100","held-book",&held));
    held.receipt.state=ReportReceipt::State::Accepted; held.receipt.seconds=199;
    held.receipt.revision=held.revision;
    assert(store::checkpoint_progress(held,held.receipt));
    assert(store::hold_pending_progress(held));
    assert(!store::load_pending_progress("100","held-book",&newer));
    assert(std::filesystem::exists(root+"/progress_313030.bin.held"));
    held.active_seconds=60;
    assert(store::save_pending_progress(held));
    assert(store::load_pending_progress("100","held-book",&held)&&held.active_seconds==60);
    assert(held.receipt.state==ReportReceipt::State::Ready); // not 259 seconds
    assert(store::hold_pending_progress(held));
    // A checkpoint can mix offline/pre-session and live time. Ack only live.
    auto mixed=a; mixed.book_id="mixed"; mixed.active_seconds=65;
    assert(store::save_pending_progress(mixed));
    assert(store::load_pending_progress("100","mixed",&mixed));
    mixed.receipt.state=ReportReceipt::State::Accepted; mixed.receipt.seconds=60;
    mixed.receipt.revision=mixed.revision;
    assert(store::checkpoint_progress(mixed,mixed.receipt));
    assert(store::acknowledge_progress(mixed));
    assert(store::load_pending_progress("100","mixed",&newer)&&newer.active_seconds==5);
    assert(newer.receipt.state==ReportReceipt::State::Ready);
    assert(!store::acknowledge_progress(mixed));
    assert(!store::hold_pending_progress(mixed)); // stale receipt cannot steal remaining time
    assert(store::hold_pending_progress(newer));
    // Simulated reboot/re-read preserves the journal; no automatic replay.
    assert(store::list_pending_progress("100",&records)&&records.size()==2);
    // A damaged primary must never resurrect a replayable Ready backup.
    std::ofstream(root+"/progress_313030.bin",std::ios::binary|std::ios::trunc)<<"broken";
    assert(store::list_pending_progress("100",&records));
    for(const auto& p:records) assert(p.receipt.state!=ReportReceipt::State::Ready);
    assert(!store::load_pending_progress("100","missing",&newer));
    std::puts("WeRead sync: acknowledgements, readback, weighted progress, source positions, multi-book/account queue, stale ack and recovery PASS");
}
