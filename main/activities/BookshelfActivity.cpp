// Bookshelf geometry / frosted rail adapted from read-pico-reader.
// Copyright 2026 mindreset. SPDX-License-Identifier: Apache-2.0
// Local activity integration and localization: MyPaper contributors.
#include "activities/BookshelfActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/HomeActivity.h"
#include "activities/SettingActivity.h"
#include "activities/recent_books.h"
#include "ui/BookshelfLayout.h"
#include "ui/BookshelfFontFit.h"
#include "ui/pico_shelf_icons.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "wifi/wifi_setting.h"
#include "time/time_basis.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <memory>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

extern void launch_file_browser(const char *path);
extern void launch_reader(const char *path);
extern void launch_epub_reader(const char *path);
extern void launch_mobi_reader(const char *path);
extern void launch_fb2_reader(const char *path);
extern void launch_markdown_reader(const char *path);
// Reuse the existing cached JPEG / software-decoder cover path.
extern bool draw_bookshelf_cover(const RecentBook &, int, int, int, int, bool *);

namespace {
namespace layout = bookshelf_layout;
using Style = layout::Style;
uint16_t gray(int v) { return ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3); }
const char *tr(const char *en, const char *cn, const char *tw) {
    auto lang = ui_locale::get();
    return lang == ui_locale::Language::English ? en :
        lang == ui_locale::Language::TraditionalChinese ? tw : cn;
}
void text(const std::string &s, int x, int y, int px, int ink = 0, int bg = 238) {
    system_font::draw_on_background(M5.Display, s, x, y, px, gray(ink), gray(bg));
}
void centered(const std::string &s, int cx, int cy, int px, int ink = 0, int bg = 238) {
    text(s, cx - system_font::measure(s, px) / 2,
         system_font::centered_y(s,cy,px), px, ink, bg);
}
int ui_px(int base) {
    return layout::ui_font_pixels(base,system_font::pixel_size());
}
int fit_px(const std::string &s, int px, int width) {
    static bookshelf_font_fit::Cache cache;
    return cache.fit(s,px,width,system_font::generation(),
        [](const std::string &value,int size) { return system_font::measure(value,size); });
}
void ui_centered(const std::string &s, int cx, int cy, int base,
                 int width, int ink = 0, int bg = 238) {
    centered(s,cx,cy,fit_px(s,ui_px(base),width),ink,bg);
}
// UTF-8 safe wrapping with the selected system face, not byte truncation.
std::vector<std::string> wrap(const std::string &s, int px, int width, int lines) {
    std::vector<std::string> out;
    std::string line;
    for (size_t at = 0; at < s.size();) {
        size_t end = at + 1;
        while (end < s.size() && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) ++end;
        std::string cp = s.substr(at, end - at);
        if (!line.empty() && system_font::measure(line + cp, px) > width) {
            if ((int)out.size() + 1 == lines) {
                while (!line.empty() && system_font::measure(line + "...", px) > width) {
                    size_t last = line.size() - 1;
                    while (last && (static_cast<unsigned char>(line[last]) & 0xc0) == 0x80) --last;
                    line.resize(last);
                }
                out.push_back(line + "...");
                return out;
            }
            out.push_back(line); line.clear();
        }
        line += cp; at = end;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}
void icon(const uint8_t *mask, int cx, int cy, int size, int ink = 0, int bg = 238) {
    // Original 72px 4bpp coverage masks, sampled at pixel centers.
    const int step=(72<<16)/size, half=step/2-(1<<15);
    for (int y = 0; y < size; ++y) {
        lgfx::rgb565_t row[72];
        for (int x = 0; x < size; ++x) {
            int sx=x*step+half, sy=y*step+half;
            int ix=sx>>16, iy=sy>>16;
            auto alpha = [&](int px, int py) {
                px = std::max(0, std::min(71, px)); py = std::max(0, std::min(71, py));
                int pos = py * 72 + px;
                return (pos & 1) ? mask[pos / 2] & 15 : mask[pos / 2] >> 4;
            };
            int fx=sx&65535, fy=sy&65535;
            int upper=(alpha(ix,iy)*(65536-fx)+alpha(ix+1,iy)*fx)>>16;
            int lower=(alpha(ix,iy+1)*(65536-fx)+alpha(ix+1,iy+1)*fx)>>16;
            int a=(upper*(65536-fy)+lower*fy)>>16;
            int blended=(((bg>>4)<<4)*(15-a)+ink*a+7)/15;
            row[x]=gray((blended>>4)*17);
        }
        M5.Display.pushImage(cx-size/2, cy-size/2+y, size, 1, row);
    }
}
void glass(layout::Rect r) {
    // Snapshot first: in-place blur would make results scan-order dependent.
    auto *source = static_cast<lgfx::rgb565_t *>(heap_caps_malloc(r.w * r.h * sizeof(lgfx::rgb565_t), MALLOC_CAP_SPIRAM));
    if (!source) { M5.Display.drawRoundRect(r.x,r.y,r.w,r.h,9,gray(112)); return; }
    M5.Display.readRect(r.x,r.y,r.w,r.h,source);
    auto sample = [&](int x, int y) {
        x=std::max(0,std::min(r.w-1,x)); y=std::max(0,std::min(r.h-1,y));
        return (int(source[y*r.w+x].r5) * 15 + 15) / 31;
    };
    lgfx::rgb565_t row[644];
    for (int y=0;y<r.h;++y) {
        int inset = std::max(0,y<9 ? 9-y : y>=r.h-9 ? y-(r.h-9+1) : 0);
        for (int x=inset;x<r.w-inset;++x) {
            int sum = sample(x,y)*4;
            for(int dy=-6;dy<=6;dy+=6) for(int dx=-6;dx<=6;dx+=6) sum+=sample(x+dx,y+dy);
            row[x-inset]=gray(((sum/13*62+10*38+50)/100)*17);
        }
        M5.Display.pushImage(r.x+inset,r.y+y,r.w-2*inset,1,row);
    }
    heap_caps_free(source);
    M5.Display.drawFastHLine(r.x+8,r.y+1,r.w-16,gray(224));
    M5.Display.drawFastHLine(r.x+8,r.y+r.h-2,r.w-16,gray(96));
    M5.Display.drawFastVLine(r.x,r.y+6,r.h-12,gray(160));
    M5.Display.drawFastVLine(r.x+r.w-1,r.y+6,r.h-12,gray(160));
    for (int cx : {r.x+16,r.x+r.w-17}) {
        M5.Display.drawCircle(cx,r.y+r.h-18,5,gray(112));
        M5.Display.drawFastHLine(cx-2,r.y+r.h-18,5,gray(96));
    }
}

class BookshelfActivity final : public Activity {
    // Accumulate during drawing; emit only after submitting the refresh.
    struct DrawTiming {
        int64_t cover_us = 0, fallback_us = 0, progress_us = 0;
        std::array<int64_t, 9> slot_us{};
        unsigned cover_ok = 0, cover_missing = 0, bin_hits = 0;
    } draw_timing;
    std::vector<RecentBook> books;
    std::vector<RecentBookProgress> progress;
    std::vector<bool> progress_loaded;
    Style style = Style::Acrylic;
    int focus = 0;
    bool shown = false, styles_open = false;
    bool keyboard_focus = false, last_wifi = false;
    std::string last_clock;
    int capacity() const { return layout::capacity(style); }
    int page() const { return focus / capacity(); }
    int pages() const { return std::max(1,(int(books.size())+capacity()-1)/capacity()); }
    void reload() {
        std::string path = focus < int(books.size()) ? books[focus].path : "";
        recent_books_load(&books);
        progress.assign(books.size(), RecentBookProgress{});
        progress_loaded.assign(books.size(), false);
        focus=0;
        for (size_t i=0;i<books.size();++i) {
            if(books[i].path==path) focus=i;
        }
    }
    void turn(int delta) {
        int p=std::max(0,std::min(pages()-1,page()+delta));
        if(p!=page()) { focus=p*capacity(); draw(false); }
    }
    void open(int i) {
        if(i<0 || i>=int(books.size())) return;
        focus=i; const auto &b=books[i];
        if(b.type=="epub" || b.type=="docx" || b.type=="comic") launch_epub_reader(b.path.c_str());
        else if(b.type=="mobi") launch_mobi_reader(b.path.c_str());
        else if(b.type=="fb2") launch_fb2_reader(b.path.c_str());
        else if(b.type=="markdown") launch_markdown_reader(b.path.c_str());
        else launch_reader(b.path.c_str());
    }
    void set_style(Style s) {
        nvs_handle_t h;
        esp_err_t err=nvs_open("bookshelf_ui",NVS_READWRITE,&h);
        if(err==ESP_OK) {
            err=nvs_set_u8(h,"style",static_cast<uint8_t>(s));
            if(err==ESP_OK) err=nvs_commit(h);
            nvs_close(h);
        }
        if(err!=ESP_OK) ESP_LOGW("BOOKSHELF","style persistence failed: %s",esp_err_to_name(err));
        style=s; styles_open=false; draw(false);
    }
    void cover(int slot,int i) {
        auto r=layout::cover(style,slot);
        M5.Display.fillRect(r.x,r.y,r.w,r.h,TFT_WHITE);
        const int64_t cover_begin = esp_timer_get_time();
        bool bin_hit = false;
        const bool cover_ok = draw_bookshelf_cover(books[i],r.x,r.y,r.w,r.h,&bin_hit);
        if(bin_hit) ++draw_timing.bin_hits;
        const int64_t cover_end = esp_timer_get_time();
        draw_timing.cover_us += cover_end - cover_begin;
        if (cover_ok) ++draw_timing.cover_ok; else ++draw_timing.cover_missing;
        if(!cover_ok) {
            const int px=ui_px(26), step=std::max(px+5,system_font::line_height(px));
            auto lines=wrap(books[i].title,px,r.w-22,std::max(1,std::min(5,(r.h-20)/step)));
            int cy=r.y+r.h/2-int(lines.size())*step/2;
            for(auto &s:lines) { centered(s,r.x+r.w/2,cy+step/2,px,0,255); cy+=step; }
        }
        draw_timing.fallback_us += esp_timer_get_time() - cover_end;
        M5.Display.drawRect(r.x,r.y,r.w,r.h,TFT_BLACK);
    }
    void list_row(int slot,int i) {
        if (!progress_loaded[i]) {
            const int64_t begin = esp_timer_get_time();
            progress[i]=recent_book_progress(books[i]);
            progress_loaded[i]=true;
            draw_timing.progress_us += esp_timer_get_time() - begin;
        }
        auto r=layout::card(style,slot); cover(slot,i);
        auto lines=wrap(books[i].title,27,378,2);
        for(size_t j=0;j<lines.size();++j)
            text(lines[j],244,system_font::centered_y(lines[j],r.y+42+int(j)*35,27),27,0,255);
        char duration[128];
        std::snprintf(duration,sizeof(duration),ui_locale::text(ui_locale::TextId::ReadingTimeFormat),
            (unsigned long long)(progress[i].reading_seconds/3600),
            (unsigned long long)(progress[i].reading_seconds/60%60));
        // Existing metadata has no author field; retain useful reading time.
        auto small=wrap(duration,21,378,1);
        if(!small.empty()) text(small[0],244,system_font::centered_y(small[0],r.y+(lines.size()>1?106:83),21),21,96,255);
        int pct=std::max(0,std::min(100,progress[i].percent));
        char label[96];
        std::snprintf(label,sizeof(label),tr("Read %d%%","已读 %d%%","已讀 %d%%"),pct);
        if(pct==0) std::snprintf(label,sizeof(label),"%s",tr("Not started · 0%","未开始 · 0%","未開始 · 0%"));
        if(pct==100) std::snprintf(label,sizeof(label),"%s",tr("Finished · 100%","已读完 · 100%","已讀完 · 100%"));
        text(label,244,system_font::centered_y(label,r.y+143,19),19,0,255);
        M5.Display.fillRoundRect(244,r.y+163,378,11,5,gray(208));
        int filled=378*pct/100;
        if(filled) M5.Display.fillRoundRect(244,r.y+163,filled,11,std::min(5,filled/2),gray(64));
        M5.Display.drawRoundRect(244,r.y+163,378,11,5,gray(80));
        if(slot<3) M5.Display.drawFastHLine(50,r.y+196,584,gray(96));
    }
    void status() {
        char clock[24]="--:--";
        if(time_basis_system_clock_valid()) time_basis_format_clock(std::time(nullptr),clock,sizeof(clock));
        last_clock=clock; last_wifi=wifi_setting_is_enabled();
        text(clock,36,system_font::centered_y(clock,40,28),28);
        const int wifi_x=570;
        icon(pico_shelf_icons::wifi,wifi_x,40,28,wifi_setting_is_enabled()?0:144);
        if(!wifi_setting_is_enabled()) M5.Display.drawLine(wifi_x-12,28,wifi_x+12,52,gray(96));
        M5.Display.drawRect(606,29,40,22,TFT_BLACK);
        M5.Display.fillRect(648,36,5,8,TFT_BLACK);
        // Battery icon only: no percentage without a reliable measurement.
    }
    void header() {
        status();
        text(ui_locale::text(ui_locale::TextId::Bookshelf),36,90,ui_px(52));
        for(auto r:{layout::manage,layout::import}) {
            M5.Display.fillRoundRect(r.x,r.y,r.w,r.h,18,TFT_WHITE);
            M5.Display.drawRoundRect(r.x,r.y,r.w,r.h,18,gray(112));
        }
        ui_centered(tr("Style","管理","管理"),490,121,20,layout::manage.w-12,0,255);
        ui_centered(tr("+ Add","+ 导入","+ 匯入"),599,121,20,layout::import.w-12,0,255);
        M5.Display.drawFastHLine(36,195,612,gray(104));
        M5.Display.drawFastHLine(36,196,612,gray(104));
    }
    void footer() {
        char s[96]; std::snprintf(s,sizeof(s),tr("%d books · %02d/%02d","%d本书 · %02d/%02d","%d本書 · %02d/%02d"),
            int(books.size()),page()+1,pages());
        const int pager_px=fit_px(s,ui_px(17),180);
        centered(s,342,1065,pager_px);
        icon(pico_shelf_icons::chevron_left,226,1065,22,page()?0:144);
        icon(pico_shelf_icons::chevron_right,458,1065,22,page()+1<pages()?0:144);
        M5.Display.drawFastHLine(0,1096,684,gray(192));
        M5.Display.fillRect(228,1097,56,5,TFT_BLACK);
        const uint8_t *icons[]={pico_shelf_icons::house,pico_shelf_icons::library_big,
            pico_shelf_icons::folder_open,pico_shelf_icons::sliders_horizontal};
        const char *labels[]={tr("Home","首页","首頁"),ui_locale::text(ui_locale::TextId::Bookshelf),
            tr("Files","文件管理","檔案管理"),ui_locale::text(ui_locale::TextId::Settings)};
        for(int i=0;i<4;++i) {
            int cx=85+171*i;
            icon(icons[i],cx,1141,44);
            const int px=fit_px(labels[i],ui_px(17),159);
            text(labels[i],cx-system_font::measure(labels[i],px)/2,1170,px);
        }
    }
    void style_dialog() {
        M5.Display.fillRoundRect(90,370,504,432,20,TFT_WHITE);
        M5.Display.drawRoundRect(90,370,504,432,20,TFT_BLACK);
        ui_centered(tr("Bookshelf style","书架样式","書架樣式"),342,411,28,460,0,255);
        const char *labels[]={tr("Acrylic","亚克力书架","壓克力書架"),tr("Dark rail","深色书轨","深色書軌"),tr("List","列表书架","列表書架")};
        const Style values[]={Style::Acrylic,Style::DarkRail,Style::List};
        for(int i=0;i<3;++i) {
            int y=451+i*100;
            if(style==values[i]) M5.Display.fillRect(112,y+20,5,40,TFT_BLACK);
            ui_centered(labels[i],342,y+40,28,440,0,255);
            if(i<2) M5.Display.drawFastHLine(112,y+90,460,gray(192));
        }
        ui_centered(tr("Tap outside to close","点击空白处关闭","點選空白處關閉"),342,775,17,460,96,255);
    }
    void show_styles() {
        styles_open=true;
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_hd);
        style_dialog();
        M5.Display.display(90,370,504,432);
        M5.Display.endWrite();
    }
    void draw(bool full = true) {
        const int64_t begin = esp_timer_get_time();
        draw_timing = {};
        M5.Display.startWrite(); M5.Display.setDither(false);
        M5.Display.setEpdMode(epd_mode_t::epd_hd);
        int32_t clip_x, clip_y, clip_w, clip_h;
        M5.Display.getClipRect(&clip_x,&clip_y,&clip_w,&clip_h);
        // Keep status/header and bottom navigation physically unchanged when
        // paging or changing style. Redraw whole rails to avoid cumulative blur.
        const int top = full ? 0 : 197;
        const int height = full ? 1216 : 899;
        M5.Display.setClipRect(0,top,684,height);
        const int64_t setup_end = esp_timer_get_time();
        M5.Display.fillRect(0,top,684,height,gray(238));
        const int64_t clear_end = esp_timer_get_time();
        if(full) header();
        const int64_t header_end = esp_timer_get_time();
        if(style==Style::List) {
            M5.Display.fillRoundRect(36,216,612,788,20,TFT_WHITE);
            M5.Display.drawRoundRect(36,216,612,788,20,gray(80));
        }
        int first=page()*capacity();
        for(int slot=0;slot<capacity() && first+slot<int(books.size());++slot) {
            const int64_t slot_begin = esp_timer_get_time();
            if(style==Style::List) list_row(slot,first+slot); else cover(slot,first+slot);
            draw_timing.slot_us[slot] = esp_timer_get_time() - slot_begin;
        }
        const int64_t items_end = esp_timer_get_time();
        if(style!=Style::List) for(int row=0;row<3;++row) {
            if(style==Style::Acrylic) glass(layout::rail(row));
            else {
                int y=438+282*row;
                M5.Display.fillRect(36,y,612,12,gray(48));
                M5.Display.drawFastHLine(36,y,612,gray(88));
                M5.Display.drawFastHLine(36,y+11,612,gray(16));
            }
        }
        const int64_t rails_end = esp_timer_get_time();
        if(books.empty()) ui_centered(ui_locale::text(ui_locale::TextId::BookshelfEmpty),342,270,28,600,0,style==Style::List?255:238);
        if(keyboard_focus && !books.empty()) {
            auto r=layout::cover(style,focus%capacity());
            M5.Display.drawRect(r.x-3,r.y-3,r.w+6,r.h+6,TFT_BLACK);
        }
        footer(); if(styles_open) style_dialog();
        M5.Display.setClipRect(clip_x,clip_y,clip_w,clip_h);
        const int64_t footer_end = esp_timer_get_time();
        M5.Display.display(0,top,684,height); M5.Display.endWrite();
        const int64_t end = esp_timer_get_time();
        char slots[192] = {};
        size_t at = 0;
        for (int slot=0;slot<capacity() && first+slot<int(books.size());++slot) {
            const int n = std::snprintf(slots+at,sizeof(slots)-at,"%s%lld",
                slot ? "/" : "", (long long)draw_timing.slot_us[slot]);
            if (n < 0 || size_t(n) >= sizeof(slots)-at) break;
            at += size_t(n);
        }
        ESP_LOGI("BOOKSHELF", "draw timing us: style=%d full=%u page=%d setup=%lld clear=%lld header=%lld items=%lld rails=%lld footer_overlay=%lld submit=%lld total=%lld cover=%lld fallback=%lld progress=%lld covers=%u/%u bin_hits=%u slots=[%s]",
            int(style),unsigned(full),page()+1,
            (long long)(setup_end-begin),(long long)(clear_end-setup_end),
            (long long)(header_end-clear_end),(long long)(items_end-header_end),
            (long long)(rails_end-items_end),(long long)(footer_end-rails_end),
            (long long)(end-footer_end),(long long)(end-begin),
            (long long)draw_timing.cover_us,(long long)draw_timing.fallback_us,
            (long long)draw_timing.progress_us,draw_timing.cover_ok,
            draw_timing.cover_missing,draw_timing.bin_hits,slots);
    }
public:
    ~BookshelfActivity() override {
        // Request only on final exit, not while pushing a reader or settings.
        request_home_quality_refresh();
    }
    void on_create() override {
        const int64_t begin = esp_timer_get_time();
        nvs_handle_t h; uint8_t value=2;
        if(nvs_open("bookshelf_ui",NVS_READONLY,&h)==ESP_OK) { nvs_get_u8(h,"style",&value); nvs_close(h); }
        if(value==1 || value==2 || value==5) style=static_cast<Style>(value);
        const int64_t settings_end = esp_timer_get_time();
        reload();
        const int64_t reload_end = esp_timer_get_time();
        draw();
        ESP_LOGI("BOOKSHELF", "open timing us: settings=%lld reload=%lld before_draw=%lld books=%u",
            (long long)(settings_end-begin),(long long)(reload_end-settings_end),
            (long long)(reload_end-begin),unsigned(books.size()));
    }
    void on_show() override { if(shown) { reload(); draw(); } else shown=true; }
    void loop() override {
        if(styles_open || M5.Display.displayBusy()) return;
        char clock[24]="--:--";
        if(time_basis_system_clock_valid()) time_basis_format_clock(std::time(nullptr),clock,sizeof(clock));
        if(last_clock==clock && last_wifi==wifi_setting_is_enabled()) return;
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_hd);
        M5.Display.fillRect(0,0,684,72,gray(238));
        status();
        M5.Display.display(0,0,684,72);
        M5.Display.endWrite();
    }
    bool on_touch(const TouchEvent &e) override {
        if(e.type==TouchEventType::Back) {
            if(styles_open) { styles_open=false; draw(false); } else ActivityManager::instance().pop();
            return true;
        }
        if(styles_open) {
            if(e.type==TouchEventType::Tap) {
                const Style values[]={Style::Acrylic,Style::DarkRail,Style::List};
                for(int i=0;i<3;++i) if(layout::Rect{112,451+i*100,460,90}.contains(e.x,e.y)) { set_style(values[i]); return true; }
                styles_open=false; draw(false);
            }
            return true;
        }
        if(e.type==TouchEventType::SwipeLeft || e.type==TouchEventType::SwipeUp) { turn(1); return true; }
        if(e.type==TouchEventType::SwipeRight || e.type==TouchEventType::SwipeDown) { turn(-1); return true; }
        if(e.type!=TouchEventType::Tap) return false;
        keyboard_focus=false;
        if(e.y>=1096) {
            int tab=std::max(0,std::min(3,e.x/171));
            if(tab==0) ActivityManager::instance().pop();
            else if(tab==2) launch_file_browser("/sdcard");
            else if(tab==3) launch_settings();
            return true;
        }
        if(layout::manage.contains(e.x,e.y)) { show_styles(); return true; }
        if(layout::import.contains(e.x,e.y)) { launch_file_browser("/sdcard/books"); return true; }
        if(layout::previous.contains(e.x,e.y)) { turn(-1); return true; }
        if(layout::next.contains(e.x,e.y)) { turn(1); return true; }
        for(int slot=0;slot<capacity();++slot)
            if(layout::card(style,slot).contains(e.x,e.y)) { open(page()*capacity()+slot); return true; }
        return false;
    }
    bool on_key(int key) override {
        if(key==APP_KEY_BACK) { TouchEvent e; e.type=TouchEventType::Back; return on_touch(e); }
        if(styles_open) return true;
        if(key==APP_KEY_UP || key==APP_KEY_DOWN) {
            if(!books.empty()) {
                focus=std::max(0,std::min(int(books.size())-1,focus+(key==APP_KEY_UP?-1:1)));
                keyboard_focus=true; draw(false);
            }
            return true;
        }
        if(key==APP_KEY_OK) { open(focus); return true; }
        return false;
    }
};
}
void launch_bookshelf() { ActivityManager::instance().push(new BookshelfActivity()); }
