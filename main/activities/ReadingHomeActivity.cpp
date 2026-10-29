// Reading dashboard geometry adapted from read-pico-reader app_dashboard.c.
// Copyright 2026 mindreset. SPDX-License-Identifier: Apache-2.0
#include "activities/ReadingHomeActivity.h"
#include "ui/ShelfStatusBar.h"
#include "ui/ReadingNavigation.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/BookshelfActivity.h"
#include "activities/FileBrowserActivity.h"
#include "activities/SettingActivity.h"
#include "activities/HomeActivity.h"
#include "activities/recent_books.h"
#include "parser/epub_parser.h"
#include "ui/ReadingHomeLayout.h"
#include "ui/BookshelfFontFit.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "time/time_basis.h"
#include <M5Unified.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <memory>
#include <sys/stat.h>
#include <esp_heap_caps.h>

extern bool draw_bookshelf_cover(const RecentBook&,int,int,int,int,bool*);
extern void launch_reader(const char*);
extern void launch_epub_reader(const char*);
extern void launch_mobi_reader(const char*);
extern void launch_fb2_reader(const char*);
extern void launch_markdown_reader(const char*);

namespace {
namespace layout=reading_home_layout;
using ui_locale::TextId;
Activity *s_home=nullptr;
uint16_t gray(int v) { return ((v>>3)<<11)|((v>>2)<<5)|(v>>3); }
const char *tr(TextId id) { return ui_locale::text(id); }
int ui_px(int base) { return bookshelf_layout::ui_font_pixels(base,system_font::pixel_size()); }
int fit_px(const std::string &s,int px,int width) {
    static bookshelf_font_fit::Cache cache;
    return cache.fit(s,px,width,system_font::generation(),
        [](const std::string &value,int pixels){return system_font::measure(value,pixels);});
}
std::string truncate(std::string value,int px,int width) {
    while(!value.empty() && system_font::measure(value,px)>width) {
        size_t last=value.size()-1;
        while(last && (static_cast<unsigned char>(value[last])&0xc0)==0x80) --last;
        value.resize(last);
    }
    return value;
}
void text(const std::string &s,int x,int y,int px,int bg=255) {
    system_font::draw_on_background(M5.Display,s,x,y,px,TFT_BLACK,gray(bg));
}
void left(const std::string &s,int x,int cy,int px,int width) {
    px=fit_px(s,px,width);
    text(s,x,system_font::centered_y(s,cy,px),px);
}
void right(const std::string &s,int x,int cy,int px,int width) {
    px=fit_px(s,px,width);
    text(s,x-system_font::measure(s,px),system_font::centered_y(s,cy,px),px);
}
void centered(const std::string &s,int x,int cy,int px,int width,int bg=255) {
    px=fit_px(s,px,width);
    text(s,x-system_font::measure(s,px)/2,system_font::centered_y(s,cy,px),px,bg);
}

class ReadingHomeActivity final : public Activity {
    RecentBook book_;
    RecentBookProgress progress_;
    std::string metadata_path_,author_,metadata_title_,clock_;
    int64_t metadata_size_=-1,metadata_mtime_=-1,today_=0;
    std::array<uint64_t,7> days_{};
    bool shown_=false,days_valid_=false;
    uint32_t status_ms_=0;
    std::string cover_path_;
    int64_t cover_size_=-1,cover_mtime_=-1;
    std::unique_ptr<uint8_t,decltype(&heap_caps_free)> cover_pixels_{nullptr,heap_caps_free};

    void reload() {
        recent_book_load(&book_);
        progress_=recent_book_progress(book_);
        struct stat source{};
        const bool source_exists=stat(book_.path.c_str(),&source)==0;
        if(book_.path!=metadata_path_ || !source_exists ||
           int64_t(source.st_size)!=metadata_size_ || int64_t(source.st_mtime)!=metadata_mtime_) {
            metadata_path_=book_.path; metadata_size_=source.st_size; metadata_mtime_=source.st_mtime;
            metadata_title_.clear(); author_.clear();
            if(book_.type=="epub") EpubBook::brief_metadata(book_.path.c_str(),&metadata_title_,&author_);
        }
        days_valid_=recent_reading_days(&days_,&today_);
        struct stat cover{};
        const bool cover_exists=stat(book_.cover_path.c_str(),&cover)==0;
        if(book_.cover_path!=cover_path_ || !cover_exists ||
           int64_t(cover.st_size)!=cover_size_ || int64_t(cover.st_mtime)!=cover_mtime_) {
            cover_pixels_.reset();
            cover_path_=book_.cover_path; cover_size_=cover.st_size; cover_mtime_=cover.st_mtime;
        }
    }
    void cover() {
        const auto r=layout::cover;
        bool ok=cover_pixels_ && M5.Display.getPanel()->writeNativeGrayscale4Rect(
            cover_pixels_.get(),r.w,r.h,r.x,r.y);
        if(!ok) {
            bool bin=false;
            ok=draw_bookshelf_cover(book_,r.x,r.y,r.w,r.h,&bin);
            if(ok) {
                cover_pixels_.reset(static_cast<uint8_t*>(heap_caps_malloc(r.w*r.h/2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)));
                if(cover_pixels_) {
                    std::array<lgfx::rgb565_t,172> row{};
                    for(int y=0;y<r.h;++y) {
                        M5.Display.readRect(r.x,r.y+y,r.w,1,row.data());
                        for(int x=0;x<r.w;x+=2) {
                            auto value=[&](int index) {
                                const auto color=static_cast<uint16_t>(row[index]);
                                return ((color>>11)*15+15)/31;
                            };
                            cover_pixels_.get()[y*r.w/2+x/2]=uint8_t(value(x)<<4|value(x+1));
                        }
                    }
                }
            }
        }
        if(!ok) {
            M5.Display.fillRect(r.x,r.y,r.w,r.h,TFT_WHITE);
            const std::string label=truncate(book_.title,24,r.w-20);
            centered(label,r.x+r.w/2,r.y+r.h/2,24,r.w-20);
            M5.Display.drawRect(r.x,r.y,r.w,r.h,TFT_BLACK);
        }
    }
    void status() {
        clock_=shelf_status_bar::draw();
        status_ms_=lgfx::millis();
    }
    void nav() {
        reading_navigation::draw(reading_navigation::Tab::Home);
    }
    void resume_area() {
        left(tr(TextId::ReadingHomeRecent),36,237,ui_px(29),380);
        if(book_.path.empty()) {
            centered(tr(TextId::ReadingHomeStartHint),342,418,ui_px(23),600);
            return;
        }
        cover();
        // Original title renderer is fixed at 40px, unlike regular UI labels.
        const std::string title=truncate(metadata_title_.empty()?book_.title:metadata_title_,40,374);
        left(title,248,322,layout::title_pixels,374);
        if(!author_.empty()) left(truncate(author_,ui_px(22),374),248,378,ui_px(22),374);
        char detail[96];
        const unsigned percent=unsigned(std::clamp(progress_.percent,0,100));
        std::snprintf(detail,sizeof(detail),tr(TextId::ReadingHomeReadPercent),percent);
        left(detail,248,449,ui_px(22),374);
        const int detail_h=system_font::ink_height(detail,ui_px(22));
        const int bar_y=layout::progress_y(detail_h);
        M5.Display.fillRoundRect(248,bar_y,374,9,5,gray(176));
        const int filled=374*percent/100;
        if(filled) M5.Display.fillRoundRect(248,bar_y,filled,9,std::min(5,filled/2),gray(56));
        const std::string action=std::string(tr(TextId::ReadingHomeContinue))+" ›";
        left(action,248,layout::continue_y(detail_h,system_font::ink_height(action,ui_px(24))),ui_px(24),374);
    }
    void statistics() {
        uint64_t sum=0,maximum=0;
        for(auto value:days_) { sum+=value; maximum=std::max(maximum,value); }
        char value[128];
        std::snprintf(value,sizeof(value),tr(sum>=3600?TextId::ReadingHomeTotalHours:TextId::ReadingHomeTotalMinutes),
            (unsigned long long)(sum/(sum>=3600?3600:60)));
        const std::string total=value;
        const uint64_t average=(sum+210)/420;
        if(average>=60) std::snprintf(value,sizeof(value),tr(TextId::ReadingHomeAverageHours),
            (unsigned long long)(average/60),(unsigned long long)(average%60));
        else std::snprintf(value,sizeof(value),tr(TextId::ReadingHomeAverageMinutes),(unsigned long long)average);
        const auto geometry=layout::statistics(
            std::max(system_font::ink_height(tr(TextId::ReadingHomeWeek),ui_px(29)),system_font::ink_height(total,ui_px(20))),
            std::max(system_font::ink_height(tr(TextId::ReadingHomeDaily),ui_px(22)),system_font::ink_height(value,ui_px(19))));
        left(tr(TextId::ReadingHomeWeek),36,geometry.heading_y,ui_px(29),350);
        right(total,648,geometry.heading_y,ui_px(20),260);
        const auto r=geometry.card;
        M5.Display.fillRoundRect(r.x,r.y,r.w,r.h,25,TFT_WHITE);
        M5.Display.drawRoundRect(r.x,r.y,r.w,r.h,25,gray(80));
        M5.Display.drawRoundRect(r.x+1,r.y+1,r.w-2,r.h-2,24,gray(80));
        left(tr(TextId::ReadingHomeDaily),62,geometry.header_y,ui_px(22),280);
        right(value,628,geometry.header_y,ui_px(19),270);
        if(!days_valid_) {
            centered(tr(TextId::ReadingHomeDateHint),342,867,ui_px(22),560);
            return;
        }
        const uint64_t hours=layout::chart_hours(maximum);
        const int available=947-geometry.grid_top;
        const int grids[]={geometry.grid_top,geometry.grid_top+available/3,geometry.grid_top+available*2/3};
        for(int i=0;i<3;++i) {
            std::snprintf(value,sizeof(value),"%lluh",(unsigned long long)(hours*(3-i)/3));
            left(value,61,grids[i],ui_px(18),40);
            M5.Display.drawFastHLine(102,grids[i],520,gray(144));
        }
        M5.Display.drawFastHLine(102,947,520,gray(144));
        static const char *week_cn[]={"日","一","二","三","四","五","六"};
        static const char *week_en[]={"Su","Mo","Tu","We","Th","Fr","Sa"};
        for(int i=0;i<7;++i) {
            const int cx=127+i*80,bar_h=layout::bar_height(days_[i],hours,available);
            if(bar_h) M5.Display.fillRoundRect(cx-18,947-bar_h,36,bar_h,std::min(18,bar_h/2),gray(56));
            const int weekday=int((today_-6+i+4)%7);
            centered(ui_locale::get()==ui_locale::Language::English?week_en[weekday]:week_cn[weekday],
                cx,977,ui_px(19),72);
        }
    }
    void draw() {
        M5.Display.startWrite(); M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_hd);
        M5.Display.fillScreen(TFT_WHITE);
        status();
        text(tr(TextId::FileNavHome),36,90,ui_px(52));
        for(int y:{195,619}) {
            M5.Display.drawFastHLine(36,y,612,gray(104));
            M5.Display.drawFastHLine(36,y+1,612,gray(104));
        }
        resume_area(); statistics(); nav();
        M5.Display.display(); M5.Display.endWrite();
    }
    void open_book() {
        if(book_.path.empty()) { launch_bookshelf(); return; }
        if(book_.type=="epub" || book_.type=="docx" || book_.type=="comic") launch_epub_reader(book_.path.c_str());
        else if(book_.type=="mobi") launch_mobi_reader(book_.path.c_str());
        else if(book_.type=="fb2") launch_fb2_reader(book_.path.c_str());
        else if(book_.type=="markdown") launch_markdown_reader(book_.path.c_str());
        else launch_reader(book_.path.c_str());
    }
public:
    ~ReadingHomeActivity() override {
        if(s_home==this) s_home=nullptr;
        request_home_quality_refresh();
    }
    void on_create() override { s_home=this; reload(); draw(); }
    void on_show() override { if(shown_) { reload(); draw(); } else shown_=true; }
    void loop() override {
        if(M5.Display.displayBusy()) return;
        if(time_basis_system_clock_valid()) {
            const auto today=(int64_t(std::time(nullptr))+time_basis_get_offset_minutes()*60)/86400;
            if(!days_valid_ || today_!=today) { days_valid_=recent_reading_days(&days_,&today_); draw(); return; }
        }
        char clock[24]="--:--";
        if(time_basis_system_clock_valid()) time_basis_format_clock(std::time(nullptr),clock,sizeof(clock));
        if(clock_==clock && uint32_t(lgfx::millis()-status_ms_)<60000) return;
        const auto previous=M5.Display.getEpdMode();
        M5.Display.startWrite(); M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillRect(0,0,M5.Display.width(),shelf_status_bar::height,TFT_WHITE); status();
        M5.Display.display(0,0,M5.Display.width(),shelf_status_bar::height); M5.Display.endWrite();
        M5.Display.setEpdMode(previous);
    }
    bool on_touch(const TouchEvent &event) override {
        if(event.type==TouchEventType::Back) { ActivityManager::instance().pop_to_root(); return true; }
        if(event.type!=TouchEventType::Tap) return false;
        const int tab=reading_navigation_layout::tab_at(event.x,event.y);
        if(tab>=0) {
            if(tab==1) switch_to_bookshelf();
            else if(tab==2) switch_to_reading_files();
            else if(tab==3) switch_to_reading_settings();
            return true;
        }
        if(layout::resume.contains(event.x,event.y)) { open_book(); return true; }
        return false;
    }
    bool on_key(int key) override {
        if(key==APP_KEY_BACK) { ActivityManager::instance().pop_to_root(); return true; }
        if(key==APP_KEY_OK) { open_book(); return true; }
        return false;
    }
};
}
void launch_reading_home() {
    auto &manager=ActivityManager::instance();
    if(s_home && manager.pop_to(s_home)) return;
    manager.show_above_root(new ReadingHomeActivity());
}
