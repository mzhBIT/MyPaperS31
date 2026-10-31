// Bookshelf geometry / frosted rail adapted from read-pico-reader.
// Copyright 2026 mindreset. SPDX-License-Identifier: Apache-2.0
// Local activity integration and localization: MyPaper contributors.
#include "activities/BookshelfActivity.h"
#include "ui/BatteryStatus.h"
#include "ui/ShelfStatusBar.h"
#include "ui/ReadingNavigation.h"
#include "activities/FileBrowserActivity.h"
#include "activities/ReadingHomeActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/HomeActivity.h"
#include "activities/SettingActivity.h"
#include "activities/recent_books.h"
#include "ui/BookshelfLayout.h"
#include "ui/BookshelfPress.h"
#include "renderer/ReaderPageBuffer.h"
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
#include <cstring>
#include <memory>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

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
void text(const std::string &s, int x, int y, int px, int ink = 0, int bg = 255) {
    system_font::draw_on_background(M5.Display, s, x, y, px, gray(ink), gray(bg));
}
void centered(const std::string &s, int cx, int cy, int px, int ink = 0, int bg = 255) {
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
                 int width, int ink = 0, int bg = 255) {
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
void icon(const uint8_t *mask, int cx, int cy, int size, int ink = 0, int bg = 255) {
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
void glass(lgfx::LGFXBase& gfx, layout::Rect r) {
    // Snapshot first: in-place blur would make results scan-order dependent.
    auto *source = static_cast<lgfx::rgb565_t *>(heap_caps_malloc(r.w * r.h * sizeof(lgfx::rgb565_t), MALLOC_CAP_SPIRAM));
    if (!source) { gfx.drawRoundRect(r.x,r.y,r.w,r.h,9,gray(112)); return; }
    gfx.readRect(r.x,r.y,r.w,r.h,source);
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
        gfx.pushImage(r.x+inset,r.y+y,r.w-2*inset,1,row);
    }
    heap_caps_free(source);
    gfx.drawFastHLine(r.x+8,r.y+1,r.w-16,gray(224));
    gfx.drawFastHLine(r.x+8,r.y+r.h-2,r.w-16,gray(96));
    gfx.drawFastVLine(r.x,r.y+6,r.h-12,gray(160));
    gfx.drawFastVLine(r.x+r.w-1,r.y+6,r.h-12,gray(160));
    for (int cx : {r.x+16,r.x+r.w-17}) {
        gfx.drawCircle(cx,r.y+r.h-18,5,gray(112));
        gfx.drawFastHLine(cx-2,r.y+r.h-18,5,gray(96));
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
    bool keyboard_focus = false;
    std::string last_clock;
    uint32_t last_status_ms = 0;
    bool last_charging = false;
    using Pixels = std::unique_ptr<uint8_t,decltype(&heap_caps_free)>;
    std::array<Pixels,9> cover_pixels{{
        {nullptr,heap_caps_free},{nullptr,heap_caps_free},{nullptr,heap_caps_free},
        {nullptr,heap_caps_free},{nullptr,heap_caps_free},{nullptr,heap_caps_free},
        {nullptr,heap_caps_free},{nullptr,heap_caps_free},{nullptr,heap_caps_free}}};
    std::unique_ptr<ReaderPageBuffer> feedback_canvas;
    Pixels feedback_crop{nullptr,heap_caps_free};
    bookshelf_press::Tracker press_tracker;
    int feedback_slot = -1;
    bool deferred_touch = false;
    TouchEvent deferred_event;

    void release_feedback() {
        press_tracker.cancel(); feedback_slot=-1; deferred_touch=false;
        for(auto& pixels:cover_pixels) pixels.reset();
        feedback_canvas.reset(); feedback_crop.reset();
    }
    void capture_cover(int slot, layout::Rect r) {
        if(style==Style::List) return;
        auto& pixels=cover_pixels[slot];
        if(!pixels) pixels.reset(static_cast<uint8_t*>(heap_caps_malloc(
            bookshelf_press::cover_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)));
        if(!pixels) return; // Normal taps continue without visual feedback.
        std::array<lgfx::rgb565_t,164> row;
        for(int y=0;y<r.h;++y) {
            M5.Display.readRect(r.x,r.y+y,r.w,1,row.data());
            for(int x=0;x<r.w;x+=2) {
                const auto a=(unsigned(row[x].r5)*15+15)/31;
                const auto b=(unsigned(row[x+1].r5)*15+15)/31;
                pixels.get()[size_t(y)*(r.w/2)+x/2]=uint8_t(a<<4|b);
            }
        }
    }
    bool paint_press(int slot, bool raised) {
        if(style==Style::List || M5.Display.displayBusy()) return false;
        const int row=slot/3,first=page()*capacity();
        for(int other=row*3;other<row*3+3 && first+other<int(books.size());++other)
            if(!cover_pixels[other]) return false;
        const auto area=bookshelf_press::row_area(slot);
        const auto dirty=bookshelf_press::dirty_area(slot);
        if(!feedback_canvas) {
            feedback_canvas.reset(new ReaderPageBuffer());
            if(!feedback_canvas->createSprite(area.w,area.h)) { feedback_canvas.reset(); return false; }
        }
        if(!feedback_crop) feedback_crop.reset(static_cast<uint8_t*>(heap_caps_malloc(
            size_t(bookshelf_press::max_dirty_width/2)*area.h,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)));
        if(!feedback_crop) return false;
        auto& gfx=*feedback_canvas;
        gfx.setDither(false); gfx.fillScreen(TFT_WHITE);
        for(int other=row*3;other<row*3+3 && first+other<int(books.size());++other) {
            auto r=layout::cover(style,other);r.y-=area.y;
            if(raised && other==slot) r.y-=bookshelf_press::lift;
            if(!gfx.getPanel()->writeNativeGrayscale4Rect(cover_pixels[other].get(),r.w,r.h,r.x,r.y)) return false;
        }
        // Compose from clean, unblurred covers each time. Never blur a previously
        // frosted rail again, and never modify neighbouring display regions.
        if(style==Style::Acrylic) {
            auto rail=layout::rail(row);rail.y-=area.y;glass(gfx,rail);
        } else {
            const int y=438+282*row-area.y;
            gfx.fillRect(36,y,612,12,gray(48));
            gfx.drawFastHLine(36,y,612,gray(88));
            gfx.drawFastHLine(36,y+11,612,gray(16));
        }
        const auto* source=static_cast<const uint8_t*>(gfx.getBuffer());
        for(int y=0;y<dirty.h;++y)
            std::memcpy(feedback_crop.get()+size_t(y)*(dirty.w/2),
                source+size_t(y)*(area.w/2)+dirty.x/2,dirty.w/2);
        const auto previous_mode=M5.Display.getEpdMode();
        M5.Display.startWrite(); M5.Display.setEpdMode(epd_mode_t::epd_hd);
        const bool ok=M5.Display.getPanel()->writeNativeGrayscale4Rect(
            feedback_crop.get(),dirty.w,dirty.h,dirty.x,dirty.y);
        if(ok) {
            M5.Display.getPanel()->setNextRefreshChangedPixelsOnly(true);
            M5.Display.display(dirty.x,dirty.y,dirty.w,dirty.h);
        }
        M5.Display.endWrite(); M5.Display.setEpdMode(previous_mode);
        if(ok) feedback_slot=raised?slot:-1;
        return ok;
    }
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
        release_feedback();
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
        capture_cover(slot,r); // Before any acrylic rail is composited.
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
        M5.Display.fillRoundRect(244,r.y+163,378,11,5,TFT_WHITE);
        int filled=378*pct/100;
        if(filled) M5.Display.fillRoundRect(244,r.y+163,filled,11,std::min(5,filled/2),gray(64));
        M5.Display.drawRoundRect(244,r.y+163,378,11,5,gray(80));
        if(slot<3) M5.Display.drawFastHLine(50,r.y+196,584,gray(96));
    }
    void status() {
        last_clock=shelf_status_bar::draw();
        last_charging=battery_status::charging();
        last_status_ms=lgfx::millis();
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
        reading_navigation::draw(reading_navigation::Tab::Bookshelf);
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
        press_tracker.cancel(); feedback_slot=-1;
        for(auto& pixels:cover_pixels) pixels.reset();
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
        M5.Display.fillRect(0,top,684,height,TFT_WHITE);
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
            if(style==Style::Acrylic) glass(M5.Display,layout::rail(row));
            else {
                int y=438+282*row;
                M5.Display.fillRect(36,y,612,12,gray(48));
                M5.Display.drawFastHLine(36,y,612,gray(88));
                M5.Display.drawFastHLine(36,y+11,612,gray(16));
            }
        }
        const int64_t rails_end = esp_timer_get_time();
        if(books.empty()) ui_centered(ui_locale::text(ui_locale::TextId::BookshelfEmpty),342,270,28,600,0,255);
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
    void on_hide() override { release_feedback(); }
    void on_standby_enter() override {} // Keep cover feedback caches for the restored page.
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
        if(deferred_touch) {
            const auto event=deferred_event;deferred_touch=false;
            on_touch(event);return; // May open another activity or delete this one.
        }
        const int wanted=press_tracker.update(touch_input_contact(),style,page()*capacity(),int(books.size()));
        if(feedback_slot>=0 && wanted!=feedback_slot) {
            if(paint_press(feedback_slot,false)) return;
            // A rare composition/allocation failure must not strand a lifted
            // cover. Repaint the existing content area as a recovery fallback.
            draw(false); return;
        }
        if(wanted>=0 && feedback_slot!=wanted) {
            if(paint_press(wanted,true)) return;
            press_tracker.cancel(); // No allocation retry loop while held.
        }
        if(wanted>=0 || feedback_slot>=0) return;
        if(touch_input_needs_fast_poll()) return;
        char clock[24]="--:--";
        if(time_basis_system_clock_valid()) time_basis_format_clock(std::time(nullptr),clock,sizeof(clock));
        // Keep the minute cadence, but react to measured charging changes.
        // Redraw the whole status strip together, not independent icon updates.
        if(last_clock==clock && uint32_t(lgfx::millis()-last_status_ms)<60000 &&
           last_charging==battery_status::charging()) return;
        const auto previous_mode=M5.Display.getEpdMode();
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillRect(0,0,M5.Display.width(),shelf_status_bar::height,TFT_WHITE);
        status();
        M5.Display.display(0,0,M5.Display.width(),shelf_status_bar::height);
        M5.Display.endWrite();
        M5.Display.setEpdMode(previous_mode);
    }
    bool on_touch(const TouchEvent &e) override {
        // Let the press refresh finish before a turn/open mutates framebuffer
        // contents. Existing release gestures are retained, not regenerated.
        if(!styles_open && style!=Style::List && M5.Display.displayBusy()) {
            if(!deferred_touch) {deferred_event=e;deferred_touch=true;}
            return true;
        }
        if(feedback_slot>=0 && e.type==TouchEventType::Tap &&
           (!layout::card(style,feedback_slot).contains(e.start_x,e.start_y) ||
            !layout::card(style,feedback_slot).contains(e.x,e.y))) {
            // Restore before opening a small style dialog that would otherwise
            // leave the raised book visible behind it.
            deferred_event=e;deferred_touch=true;
            press_tracker.cancel();
            paint_press(feedback_slot,false);
            return true;
        }
        press_tracker.cancel();
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
        const int tab=reading_navigation_layout::tab_at(e.x,e.y);
        if(tab>=0) {
            if(tab==0) launch_reading_home();
            else if(tab==2) switch_to_reading_files();
            else if(tab==3) switch_to_reading_settings();
            return true;
        }
        if(layout::manage.contains(e.x,e.y)) { show_styles(); return true; }
        if(layout::import.contains(e.x,e.y)) { launch_file_browser("/sdcard/books", true); return true; }
        if(layout::previous.contains(e.x,e.y)) { turn(-1); return true; }
        if(layout::next.contains(e.x,e.y)) { turn(1); return true; }
        for(int slot=0;slot<capacity();++slot)
            if(layout::card(style,slot).contains(e.x,e.y)) {
                if(style!=Style::List && !layout::card(style,slot).contains(e.start_x,e.start_y)) return true;
                open(page()*capacity()+slot); return true;
            }
        return false;
    }
    bool on_navigation_key(int direction) override { return on_key(direction < 0 ? APP_KEY_UP : APP_KEY_DOWN); }
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
void switch_to_bookshelf() { ActivityManager::instance().show_above_root(new BookshelfActivity()); }
