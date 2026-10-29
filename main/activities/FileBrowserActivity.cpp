// File page geometry adapted from read-pico-reader app_files.c.
// Copyright 2026 mindreset. SPDX-License-Identifier: Apache-2.0
#include "activities/Activity.h"
#include "activities/FileBrowserActivity.h"
#include "activities/ReadingHomeActivity.h"
#include "ui/ListFocus.h"
#include "ui/FileBrowserLayout.h"
#include "ui/pico_file_icons.h"
#include "ui/pico_shelf_icons.h"
#include "ui/BookshelfLayout.h"
#include "ui/BookshelfFontFit.h"
#include "activities/BookshelfActivity.h"
#include "activities/SettingActivity.h"
#include "activities/TransferActivity.h"
#include "activities/WifiTransferActivity.h"
#include "time/time_basis.h"
#include "wifi/wifi_setting.h"
#include "ui/ShelfStatusBar.h"
#include <esp_vfs_fat.h>
#include <ctime>
#include "activities/ActivityManager.h"
#include "activities/MusicActivity.h"
#include "activities/PlaceholderActivity.h"
#include "activities/recent_books.h"
#include "audio/audio_file_formats.h"
#include "reader_font.h"
#include "ui/SystemFont.h"
#include "ui/ReadingNavigation.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"
#include "ui/TextInputOverlay.h"
#include "ui/reader_background.h"
#include "ui/standby_wallpaper.h"
#include "ui/reader_book_cover.h"
#include <M5Unified.h>
#include <string>
#include <vector>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <strings.h>
#include <esp_timer.h>

static const char *TAG = "FILES";
namespace fl = file_browser_layout;
static std::string g_path;
static bool home_view() { return g_path == "/sdcard"; }
static uint64_t g_total_bytes = 0, g_free_bytes = 0;
static bool g_storage_ready = false;
static bool g_sort_ascending = true;
static uint16_t gray(int v) { return ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3); }
static int ui_px(int px) { return bookshelf_layout::ui_font_pixels(px,system_font::pixel_size()); }
static fl::Content content_layout() {
    static bool valid=false;
    static uint32_t generation=0;
    static uint32_t locale_generation=0;
    static fl::Content cached[2];
    const auto current=system_font::generation();
    const auto locale=ui_locale::generation();
    if(!valid || generation!=current || locale_generation!=locale) {
        const int title=system_font::line_height(ui_px(34));
        const int path=system_font::line_height(ui_px(20));
        const int name=system_font::line_height(ui_px(23));
        const int detail=system_font::line_height(ui_px(19));
        const int root=system_font::line_height(ui_px(21));
        const int storage_title=system_font::line_height(ui_px(27));
        int sort_width=std::max(system_font::measure(ui_locale::text(ui_locale::TextId::FileSortAscending),ui_px(19)),
            system_font::measure(ui_locale::text(ui_locale::TextId::FileSortDescending),ui_px(19)));
        sort_width=std::max(111,std::min(260,sort_width+32));
        cached[0]=fl::content(false,title,path,name,detail,root,storage_title,sort_width);
        cached[1]=fl::content(true,title,path,name,detail,root,storage_title,sort_width);
        generation=current; locale_generation=locale; valid=true;
    }
    return cached[home_view()?1:0];
}
static int list_rows() { return content_layout().count; }
static std::string fit_text(std::string value, int width, int px) {
    if (system_font::measure(value,px) <= width) return value;
    while (!value.empty() && system_font::measure(value+"...",px) > width) {
        size_t last=value.size()-1;
        while(last && (static_cast<unsigned char>(value[last])&0xc0)==0x80) --last;
        value.resize(last);
    }
    return value+"...";
}
static void text(const std::string &s,int x,int y,int px,int bg=255) {
    system_font::draw_on_background(M5.Display,s,x,y,px,TFT_BLACK,gray(bg));
}
static void center(const std::string &s,int cx,int cy,int px,int width=600,int bg=255) {
    static bookshelf_font_fit::Cache cache;
    px=cache.fit(s,px,width,system_font::generation(),
        [](const std::string &v,int size){return system_font::measure(v,size);});
    text(s,cx-system_font::measure(s,px)/2,system_font::centered_y(s,cy,px),px,bg);
}
static void icon(const uint8_t *mask, int cx, int cy, int size, int ink = 0, int bg = 255) {
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

struct FsEntry { std::string name; bool is_dir; uint64_t size = 0; };
static std::string file_detail(const FsEntry &entry) {
    if (entry.is_dir) return ui_locale::text(ui_locale::TextId::FileFolder);
    char value[48];
    if (entry.size >= 1048576) snprintf(value,sizeof(value),"%.1f MB",entry.size/1048576.0);
    else if (entry.size >= 1024) snprintf(value,sizeof(value),"%.1f KB",entry.size/1024.0);
    else snprintf(value,sizeof(value),"%llu B",(unsigned long long)entry.size);
    return value;
}
static void file_icon(int x,int y,bool folder) {
    icon(folder ? pico_file_icons::folder : pico_file_icons::file,x+21,y+21,46);
}
static std::string g_root;
static std::vector<FsEntry> g_entries;
static int g_scroll = 0;
static int g_focus  = 0;
static bool g_keyboard_focus = false;
static bool g_restore_focus = false;
static int  g_saved_focus  = 0;
static int  g_saved_scroll = 0;

// ─── Filters ──────────────────────────────────────────────────────

static bool skip_name(const char *n) {
    if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return true;
    if (n[0] == '.') return true;
    if (strcasecmp(n, "System Volume Information") == 0) return true;
    return false;
}


static bool is_font_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && (strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".otf") == 0 ||
                   strcasecmp(ext, ".bin") == 0);
}

static bool is_zip_reflow_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && (strcasecmp(ext, ".epub") == 0 || strcasecmp(ext, ".docx") == 0 ||
                   strcasecmp(ext, ".zip") == 0 || strcasecmp(ext, ".cbz") == 0);
}

static bool is_mobi_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && (strcasecmp(ext, ".mobi") == 0 || strcasecmp(ext, ".prc") == 0 ||
                   strcasecmp(ext, ".azw") == 0 || strcasecmp(ext, ".azw3") == 0);
}

static bool is_fb2_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && strcasecmp(ext, ".fb2") == 0;
}

static bool is_markdown_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && (strcasecmp(ext, ".md") == 0 || strcasecmp(ext, ".markdown") == 0);
}

static bool is_image_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
                   strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0);
}

// ─── Drawing ───────────────────────────────────────────────────────

static void draw_status() {
    shelf_status_bar::draw();
}
static void draw_nav() {
    reading_navigation::draw(reading_navigation::Tab::Files);
}
static void draw_back() {
    icon(pico_shelf_icons::chevron_left,58,114,38);
}
static void draw_topbar() {
    draw_status();
    if(home_view()) {
        text(ui_locale::text(ui_locale::TextId::Files),36,91,52);
        auto add=fl::transfer_add;
        M5.Display.fillRect(add.x+12,add.y+26,32,3,TFT_BLACK);
        M5.Display.fillRect(add.x+26,add.y+12,3,32,TFT_BLACK);
        const auto layout=content_layout();
        auto r=layout.storage_card;
        M5.Display.drawRoundRect(r.x,r.y,r.w,r.h,24,gray(80));
        M5.Display.drawRoundRect(r.x+1,r.y+1,r.w-2,r.h-2,23,gray(80));
        char total[32],usage[128];
        if(g_storage_ready) {
            snprintf(total,sizeof(total),"%.1f GB",g_total_bytes/1073741824.0);
            snprintf(usage,sizeof(usage),ui_locale::text(ui_locale::TextId::FileStorageUsage),
                (g_total_bytes-g_free_bytes)/1073741824.0,g_free_bytes/1073741824.0);
        } else {
            snprintf(total,sizeof(total),"%s",ui_locale::text(ui_locale::TextId::FileStorageUnavailable));
            usage[0]=0;
        }
        int px=ui_px(19);
        int total_width=std::min(240,system_font::measure(total,px));
        text(fit_text(ui_locale::text(ui_locale::TextId::FileStorage),566-total_width-16,ui_px(27)),58,r.y+16,ui_px(27));
        std::string total_label=fit_text(total,total_width,px);
        text(total_label,624-system_font::measure(total_label,px),r.y+16,px);
        text(fit_text(usage,566,px),58,layout.storage_usage_y,px);
    } else {
        draw_back();
        std::string name=g_path.substr(g_path.find_last_of('/')+1);
        center(name,342,107,ui_px(34),470);
        const auto layout=content_layout();
        text(fit_text(g_path,612,ui_px(20)),36,layout.path_y,ui_px(20));
        M5.Display.drawFastHLine(36,layout.separator_y,612,gray(96));
        M5.Display.drawFastHLine(36,layout.separator_y+1,612,gray(96));
    }
}
static void draw_list() {
    const bool home=home_view();
    const auto layout=content_layout();
    const int top=layout.top,rows=layout.count;
    const int last= g_entries.empty()?0:((int(g_entries.size())-1)/rows)*rows;
    g_scroll=std::max(0,std::min(g_scroll,last));
    g_focus=std::max(0,std::min(g_focus,int(g_entries.size())-1));
    if(home) {
        char count[64];
        snprintf(count,sizeof(count),ui_locale::text(ui_locale::TextId::FileRootCount),(unsigned)g_entries.size());
        auto r=layout.sort_button;
        text(fit_text(count,r.x-36-16,ui_px(21)),36,
            layout.root_y+(layout.root_h-system_font::line_height(ui_px(21)))/2,ui_px(21));
        M5.Display.drawRoundRect(r.x,r.y,r.w,r.h,23,gray(112));
        center(ui_locale::text(g_sort_ascending?ui_locale::TextId::FileSortAscending:ui_locale::TextId::FileSortDescending),
            r.x+r.w/2,r.y+r.h/2,ui_px(19),r.w-32);
        M5.Display.drawRoundRect(36,top,612,rows*layout.row_h,24,gray(80));
        M5.Display.drawRoundRect(37,top+1,610,rows*layout.row_h-2,23,gray(80));
    }
    for(int row=0;row<rows;++row) {
        int index=g_scroll+row;
        if(index>=int(g_entries.size())) break;
        const auto &entry=g_entries[index];
        int y=top+row*layout.row_h;
        if(g_keyboard_focus) list_focus::draw(M5.Display,home?39:25,y,layout.row_h,index==g_focus);
        if(home && row) {
            M5.Display.drawFastHLine(96,y,532,gray(96));
            M5.Display.drawFastHLine(96,y+1,532,gray(96));
        }
        file_icon(home?52:48,y+layout.row_h/2-21,entry.is_dir);
        std::string label=fit_text(entry.name,home?430:450,ui_px(23));
        if(home && entry.is_dir) text(label,101,system_font::centered_y(label,y+layout.row_h/2,ui_px(23)),ui_px(23));
        else {
            text(label,101,y+12,ui_px(23));
            text(file_detail(entry),101,y+18+system_font::line_height(ui_px(23)),ui_px(19));
        }
        icon(pico_shelf_icons::chevron_right,home?624:636,y+layout.row_h/2,22);
        if(!home) {
            M5.Display.drawFastHLine(101,y+layout.row_h-2,547,gray(96));
            M5.Display.drawFastHLine(101,y+layout.row_h-1,547,gray(96));
        }
    }
    if(g_entries.empty()) center(ui_locale::text(ui_locale::TextId::WebEmptyDirectory),342,top+46,ui_px(22));
    char page[32]; snprintf(page,sizeof(page),"%d / %d",g_scroll/rows+1,last/rows+1);
    center(page,342,1057,ui_px(20));
    draw_nav();
}

static void load_dir(const std::string &path) {
    g_keyboard_focus = false;
    g_path = path;
    g_entries.clear();
    g_focus = 0;
    g_scroll = 0;

    DIR *d = opendir(path.c_str());
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (skip_name(ent->d_name)) continue;
        FsEntry e;
        e.name = ent->d_name;
        std::string full = path + "/" + ent->d_name;
        struct stat st;
        bool stat_ok = stat(full.c_str(), &st) == 0;
        e.is_dir = stat_ok && S_ISDIR(st.st_mode);
        e.size = stat_ok && st.st_size > 0 ? (uint64_t)st.st_size : 0;
        if (!stat_ok || (!e.is_dir && !S_ISREG(st.st_mode))) continue;
        g_entries.push_back(e);
    }
    closedir(d);

    // Sort: dirs first, then alphabetically
    std::sort(g_entries.begin(), g_entries.end(), [](auto &a, auto &b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return g_sort_ascending ? a.name < b.name : a.name > b.name;
    });

    if (g_restore_focus) {
        g_restore_focus = false;
        g_focus  = std::min(g_saved_focus,  (int)g_entries.size() - 1);
        g_scroll = std::min(g_saved_scroll, (int)g_entries.size() - 1);
        if (g_focus < 0) g_focus = 0;
        if (g_scroll < 0) g_scroll = 0;
    }
}

static void enter_dir(const std::string &sub) {
    g_saved_focus  = g_focus;
    g_saved_scroll = g_scroll;
    if (g_path == "/") load_dir("/" + sub);
    else load_dir(g_path + "/" + sub);
}

static void go_up() {
    if (g_path == "/" || g_path.empty()) return;
    auto pos = g_path.rfind('/');
    g_restore_focus = true; // restore focus+scroll after load
    if (pos == std::string::npos || pos == 0) load_dir("/");
    else load_dir(g_path.substr(0, pos));
}

static int hit_row(int ty) {
    int row = content_layout().row_at(ty);
    if(row < 0) return -1;
    int idx = row + g_scroll;
    return (idx < (int)g_entries.size()) ? idx : -1;
}

static bool hit_back(int tx, int ty) {
    return tx >= 26 && tx < 110 && ty >= 79 && ty < 157;
}

// ─── Activity ──────────────────────────────────────────────────────

class FileBrowserActivity : public Activity {
    bool m_initial_hd = false;
    bool m_initial_show = true;
    bool m_transfer_menu = false;
    int m_transfer_choice = 0;
    bool m_action_menu = false;
    int m_action_choice = 0;
    bool m_confirm_delete = false;
    int m_delete_choice = 0; // 0=cancel, 1=delete
    std::string m_notice;
    ui::TextInputOverlay m_text_input;

public:
    explicit FileBrowserActivity(bool initial_hd = false) : m_initial_hd(initial_hd) {}
    void draw_transfer_menu() {
        auto r=fl::transfer_menu;
        M5.Display.fillRoundRect(r.x,r.y,r.w,r.h,20,TFT_WHITE);
        M5.Display.drawRoundRect(r.x,r.y,r.w,r.h,20,TFT_BLACK);
        const char *methods[]={ui_locale::text(ui_locale::TextId::WifiTransfer),
            ui_locale::text(ui_locale::TextId::FileHotspotTransfer),ui_locale::text(ui_locale::TextId::UsbTransfer)};
        const char *details[]={ui_locale::text(ui_locale::TextId::FileSameNetwork),
            ui_locale::text(ui_locale::TextId::FileConnectHotspot),ui_locale::text(ui_locale::TextId::FileConnectPc)};
        for(int i=0;i<3;++i) {
            auto row=fl::transfer(i);
            if(i) M5.Display.drawFastHLine(row.x,row.y-4,row.w,gray(128));
            if(g_keyboard_focus) list_focus::draw(M5.Display,row.x,row.y,row.h,i==m_transfer_choice);
            center(methods[i],row.x+row.w/2,row.y+25,std::min(ui_px(21),30),row.w-32);
            center(details[i],row.x+row.w/2,row.y+66,std::min(ui_px(19),24),row.w-32);
        }
    }
    void select_transfer(int choice) {
        m_transfer_menu=false;
        if(choice==0) launch_wifi_transfer();
        else if(choice==1) launch_hotspot_transfer();
        else launch_usb_transfer();
        // Wi-Fi setup may return without pushing an activity.
        if(ActivityManager::instance().current()==this) redraw_all();
    }
    void on_create() override {
        ESP_LOGI(TAG, "create: %s", g_path.c_str());
        g_storage_ready = esp_vfs_fat_info("/sdcard",&g_total_bytes,&g_free_bytes)==ESP_OK;
        g_free_bytes=std::min(g_free_bytes,g_total_bytes);
        M5.Display.startWrite();
        M5.Display.setEpdMode(m_initial_hd ? epd_mode_t::epd_hd : epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar();
        draw_list();
        M5.Display.display();
        M5.Display.endWrite();
    }

    void on_show() override {
        // push() invokes on_create() followed by on_show(); do not submit twice.
        if(m_initial_show) { m_initial_show=false; return; }
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar();
        draw_list();
        M5.Display.display();
        M5.Display.endWrite();
    }

    std::string selected_path() const {
        if (g_focus < 0 || g_focus >= (int)g_entries.size()) return std::string();
        return g_path + "/" + g_entries[g_focus].name;
    }

    int visible_rows() const {
        return list_rows();
    }

    void go_back_or_up() {
        if (g_path == g_root || g_path == "/" || g_path.empty()) {
            ActivityManager::instance().pop();
            return;
        }
        go_up();
        redraw_all();
    }

    void page_up() {
        int rows = visible_rows();
        g_scroll = std::max(0, g_scroll - rows);
        g_focus = std::max(0, g_focus - rows);
        redraw_all();
    }

    void page_down() {
        if (g_entries.empty()) return;
        int rows = visible_rows();
        int last=((int(g_entries.size())-1)/rows)*rows;
        g_scroll=std::min(last,g_scroll+rows);
        g_focus=std::min(int(g_entries.size())-1,g_scroll);
        redraw_all();
    }

    void open_focused() {
        if (g_focus < 0 || g_focus >= (int)g_entries.size()) return;
        const auto &e = g_entries[g_focus];
        if (e.is_dir) {
            enter_dir(e.name);
            redraw_all();
            return;
        }

        std::string full = g_path + "/" + e.name;
        ESP_LOGI(TAG, "Open: %s", full.c_str());
        if (full.size() > 4 && strcasecmp(full.c_str() + full.size() - 4, ".txt") == 0) {
            extern void launch_reader(const char*);
            launch_reader(full.c_str());
        } else if (is_zip_reflow_file(full.c_str())) {
            extern void launch_epub_reader(const char*);
            launch_epub_reader(full.c_str());
        } else if (is_mobi_file(full.c_str())) {
            extern void launch_mobi_reader(const char*);
            launch_mobi_reader(full.c_str());
        } else if (is_fb2_file(full.c_str())) {
            extern void launch_fb2_reader(const char*);
            launch_fb2_reader(full.c_str());
        } else if (is_markdown_file(full.c_str())) {
            extern void launch_markdown_reader(const char*);
            launch_markdown_reader(full.c_str());
        } else if (is_image_file(full.c_str())) {
            extern void launch_image_viewer(const char*);
            launch_image_viewer(full.c_str());
        } else if (is_font_file(full.c_str())) {
            reader_font_set(full.c_str());
            redraw_all();
        } else if (audio_file_formats::is_supported(full.c_str())) {
            launch_music(full.c_str());
        } else {
            ActivityManager::instance().push(new PlaceholderActivity(full.c_str()));
        }
    }

    static std::string ellipsize_middle(const std::string &s, size_t max_len) {
        if (s.size() <= max_len) return s;
        if (max_len <= 6) return s.substr(0, max_len);
        size_t head = (max_len - 3) / 2;
        size_t tail = max_len - 3 - head;
        return s.substr(0, head) + "..." + s.substr(s.size() - tail);
    }

    bool action_has_background() const {
        return g_focus >= 0 && g_focus < (int)g_entries.size() &&
               !g_entries[g_focus].is_dir && is_image_file(g_entries[g_focus].name.c_str());
    }

    bool action_has_standby() const {
#if CONFIG_IDF_TARGET_ESP32S31
        return action_has_background();
#else
        return false;
#endif
    }
    int action_count() const { return action_has_background() ? (action_has_standby() ? 5 : 4) : 3; }

    using SheetRect = fl::Rect;
    SheetRect action_sheet() const { return fl::sheet; }
    SheetRect action_button(int index) const {
        return index==action_count()-1 ? fl::cancel : fl::action(index);
    }
    bool action_close_at(int tx,int ty) const {
        return !fl::sheet.contains(tx,ty) || fl::close.contains(tx,ty);
    }
    void draw_action_dialog() {
        auto r=fl::sheet;
        M5.Display.fillRoundRect(r.x,r.y,r.w,r.h,28,TFT_WHITE);
        M5.Display.drawRoundRect(r.x,r.y,r.w,r.h,28,gray(80));
        M5.Display.drawRoundRect(r.x+1,r.y+1,r.w-2,r.h-2,27,gray(80));
        M5.Display.fillCircle(623,473,19,gray(224));
        M5.Display.drawLine(616,466,630,480,TFT_BLACK);
        M5.Display.drawLine(630,466,616,480,TFT_BLACK);
        const auto &entry=g_entries[g_focus];
        file_icon(48,467,entry.is_dir);
        text(fit_text(entry.name,490,ui_px(28)),102,462,ui_px(28));
        text(file_detail(entry),102,507,ui_px(20));
        text(fit_text(selected_path(),585,ui_px(19)),50,552,ui_px(19));
        M5.Display.drawFastHLine(42,590,600,gray(96));
        M5.Display.drawFastHLine(42,591,600,gray(96));
        if(m_confirm_delete) {
            center(ui_locale::text(ui_locale::TextId::DeleteFileQuestion),342,712,ui_px(28),590);
            const char *confirm_labels[]={ui_locale::text(ui_locale::TextId::Cancel),
                ui_locale::text(ui_locale::TextId::Delete)};
            for(int i=0;i<2;++i) {
                int x=50+i*306;
                bool selected=i==m_delete_choice;
                M5.Display.fillRoundRect(x,788,278,76,16,selected?TFT_BLACK:TFT_WHITE);
                M5.Display.drawRoundRect(x,788,278,76,16,TFT_BLACK);
                int px=std::min(ui_px(25),32);
                std::string label=confirm_labels[i];
                system_font::draw_on_background(M5.Display,label,
                    x+(278-system_font::measure(label,px))/2,
                    system_font::centered_y(label,826,px),px,
                    selected?TFT_WHITE:TFT_BLACK,selected?TFT_BLACK:TFT_WHITE);
            }
            return;
        }
        const char *normal[]={ui_locale::text(ui_locale::TextId::Rename),
            ui_locale::text(ui_locale::TextId::Delete),ui_locale::text(ui_locale::TextId::Cancel)};
        const char *image[]={ui_locale::text(ui_locale::TextId::Rename),
            ui_locale::text(ui_locale::TextId::Background),
#if CONFIG_IDF_TARGET_ESP32S31
            ui_locale::text(ui_locale::TextId::StandbyWallpaper),
#endif
            ui_locale::text(ui_locale::TextId::Delete),
            ui_locale::text(ui_locale::TextId::Cancel)};
        auto labels=action_has_background()?image:normal;
        for(int i=0;i<action_count();++i) {
            auto button=action_button(i);
            bool cancel=i==action_count()-1;
            M5.Display.fillRoundRect(button.x,button.y,button.w,button.h,cancel?26:20,gray(cancel?224:232));
            if(!cancel) {
                M5.Display.drawRoundRect(button.x,button.y,button.w,button.h,20,gray(96));
                M5.Display.drawRoundRect(button.x+1,button.y+1,button.w-2,button.h-2,19,gray(96));
            }
            if(g_keyboard_focus && i==m_action_choice)
                M5.Display.drawRoundRect(button.x+3,button.y+3,button.w-6,button.h-6,cancel?23:17,TFT_BLACK);
            center(labels[i],button.x+button.w/2,button.y+button.h/2,ui_px(cancel?23:25),button.w-40,cancel?224:232);
        }
        center(ui_locale::text(ui_locale::TextId::FileActionsHint),342,900,ui_px(20),590);
    }
    int action_choice_at(int tx,int ty) const {
        for(int i=0;i<action_count();++i) if(action_button(i).contains(tx,ty)) return i;
        return -1;
    }

    void begin_action_menu() {
        if (g_focus < 0 || g_focus >= (int)g_entries.size()) return;
        m_action_menu = true;
        m_action_choice = 0;
        m_notice.clear();
        redraw_all();
    }

    static bool valid_file_name(const std::string &name) {
        if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
        if (name.back() == ' ' || name.back() == '.') return false;
        for (unsigned char c : name) {
            if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        }
        return true;
    }

    void rename_entry(const std::string &old_path, const std::string &new_name) {
        m_notice.clear();
        if (!valid_file_name(new_name)) {
            m_notice = ui_locale::text(ui_locale::TextId::InvalidFileName);
            return;
        }
        std::string new_path = g_path + "/" + new_name;
        if (new_path == old_path) return;
        struct stat st = {};
        bool case_only = strcasecmp(old_path.c_str(), new_path.c_str()) == 0;
        if (!case_only && stat(new_path.c_str(), &st) == 0) {
            m_notice = ui_locale::text(ui_locale::TextId::FileAlreadyExists);
            return;
        }

        int result = -1;
        if (case_only) {
            char temp_name[64];
            std::snprintf(temp_name, sizeof(temp_name), ".rename_%lld.tmp", (long long)esp_timer_get_time());
            std::string temp_path = g_path + "/" + temp_name;
            if (std::rename(old_path.c_str(), temp_path.c_str()) == 0) {
                result = std::rename(temp_path.c_str(), new_path.c_str());
                if (result != 0) std::rename(temp_path.c_str(), old_path.c_str());
            }
        } else {
            result = std::rename(old_path.c_str(), new_path.c_str());
        }
        if (result != 0) {
            ESP_LOGW(TAG, "rename failed old=%s new=%s errno=%d", old_path.c_str(), new_path.c_str(), errno);
            m_notice = ui_locale::text(ui_locale::TextId::RenameFailed);
            return;
        }

        recent_books_rename_path(old_path.c_str(), new_path.c_str());
        reader_background_file_renamed(old_path.c_str(), new_path.c_str());
        reader_book_cover_file_renamed(old_path.c_str(), new_path.c_str());
        ESP_LOGI(TAG, "renamed: %s -> %s", old_path.c_str(), new_path.c_str());
        load_dir(g_path);
        for (size_t i = 0; i < g_entries.size(); ++i) {
            if (g_entries[i].name == new_name) {
                g_focus = (int)i;
                break;
            }
        }
    }

    void begin_rename() {
        if (g_focus < 0 || g_focus >= (int)g_entries.size()) return;
        const std::string old_path = selected_path();
        TextInputOptions options;
        options.title = ui_locale::text(ui_locale::TextId::Rename);
        options.initial_text = g_entries[g_focus].name;
        options.submit_label = ui_locale::text(ui_locale::TextId::Rename);
        options.max_bytes = 255;
        options.start_in_chinese = true;
        options.text_refresh = true;
        m_text_input.open(options, [this, old_path](const std::string &name) {
            rename_entry(old_path, name);
            redraw_all();
        }, [this]() { redraw_all(); });
    }

    void handle_action(int choice) {
        bool background_action = action_has_background();
        m_action_menu = false;
        m_action_choice = 0;
        if (choice == 0) begin_rename();
        else if (background_action && choice == 1) {
            std::string message;
            ReaderBackgroundResult result = reader_background_set(
                selected_path().c_str(), M5.Display.width(), M5.Display.height(), &message);
            m_notice = message;
            ESP_LOGI(TAG, "set reading background path=%s result=%d",
                     selected_path().c_str(), (int)result);
            redraw_all();
        }
        else if (background_action && action_has_standby() && choice == 2) {
            standby_wallpaper_set(selected_path().c_str(), M5.Display.width(), M5.Display.height(), &m_notice);
            redraw_all();
        }
        else if (choice == (background_action ? (action_has_standby() ? 3 : 2) : 1)) {
            if (g_focus >= 0 && g_focus < (int)g_entries.size() && g_entries[g_focus].is_dir) {
                m_notice = ui_locale::text(ui_locale::TextId::FolderDeleteUnsupported);
                redraw_all();
            } else begin_delete_confirm();
        }
        else redraw_all();
    }

    void draw_notice() {
        if (m_notice.empty()) return;
        int w = M5.Display.width();
        int box_w = std::min(w - 48, 600);
        int x = (w - box_w) / 2;
        int y = M5.Display.height() / 2 - 70;
        M5.Display.fillRoundRect(x, y, box_w, 140, 7, TFT_WHITE);
        M5.Display.drawRoundRect(x, y, box_w, 140, 7, TFT_BLACK);
        system_font::draw_center(M5.Display, system_font::ellipsize(m_notice, box_w - 30), w / 2, y + 24, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToClose), w / 2, y + 78, TFT_BLACK);
    }

    void draw_delete_dialog() { draw_action_dialog(); }

    int delete_dialog_choice_at(int tx,int ty) const {
        if(fl::Rect{50,788,278,76}.contains(tx,ty)) return 0;
        if(fl::Rect{356,788,278,76}.contains(tx,ty)) return 1;
        return -1;
    }

    void begin_delete_confirm() {
        if (g_focus < 0 || g_focus >= (int)g_entries.size()) return;
        if (g_entries[g_focus].is_dir) return;
        m_confirm_delete = true;
        m_delete_choice = 0;
        redraw_all();
    }

    void cancel_delete_confirm() {
        m_confirm_delete = false;
        m_delete_choice = 0;
        redraw_all();
    }

    void confirm_delete() {
        if (g_focus < 0 || g_focus >= (int)g_entries.size() || g_entries[g_focus].is_dir) {
            cancel_delete_confirm();
            return;
        }

        std::string path = selected_path();
        int old_focus = g_focus;
        if (unlink(path.c_str()) == 0) {
            reader_background_file_deleted(path.c_str());
            reader_book_cover_file_deleted(path.c_str());
            ESP_LOGI(TAG, "deleted: %s", path.c_str());
            load_dir(g_path);
            if (!g_entries.empty()) {
                g_focus = std::min(old_focus, (int)g_entries.size() - 1);
                int vis_rows = list_rows();
                if (g_focus < g_scroll) g_scroll = g_focus;
                if (g_focus >= g_scroll + vis_rows) g_scroll = std::max(0, g_focus - vis_rows + 1);
            }
        } else {
            ESP_LOGW(TAG, "delete failed: %s errno=%d", path.c_str(), errno);
        }
        m_confirm_delete = false;
        m_delete_choice = 0;
        redraw_all();
    }

    bool on_touch(const TouchEvent &event) override {
        if (m_text_input.visible()) return m_text_input.handle_touch(event);
        if (event.type == TouchEventType::Back) {
            if (m_transfer_menu) { m_transfer_menu=false; redraw_all(); }
            else if (!m_notice.empty()) { m_notice.clear(); redraw_all(); }
            else if (m_action_menu) { m_action_menu = false; redraw_all(); }
            else if (m_confirm_delete) cancel_delete_confirm();
            else go_back_or_up();
            return true;
        }

        if(m_transfer_menu) {
            if(event.type!=TouchEventType::Tap) return true;
            if(!fl::transfer_menu.contains(event.x,event.y)) {
                m_transfer_menu=false; redraw_all(); return true;
            }
            for(int i=0;i<3;++i) if(fl::transfer(i).contains(event.x,event.y)) {
                select_transfer(i); return true;
            }
            return true;
        }

        if (!m_notice.empty()) {
            if (event.type == TouchEventType::Tap) { m_notice.clear(); redraw_all(); }
            return true;
        }

        if (m_action_menu) {
            if (event.type != TouchEventType::Tap) return true;
            if (action_close_at(event.x, event.y)) { m_action_menu = false; redraw_all(); return true; }
            int choice = action_choice_at(event.x, event.y);
            if (choice >= 0) handle_action(choice);
            return true;
        }

        if (m_confirm_delete) {
            if (event.type != TouchEventType::Tap) return true;
            if(action_close_at(event.x,event.y)) { cancel_delete_confirm(); return true; }
            int choice = delete_dialog_choice_at(event.x, event.y);
            if (choice >= 0) {
                m_delete_choice = choice;
                if (m_delete_choice == 1) confirm_delete();
                else cancel_delete_confirm();
            }
            return true;
        }

        if (event.type == TouchEventType::SwipeUp || event.type == TouchEventType::SwipeLeft) {
            page_down();
            return true;
        }
        if (event.type == TouchEventType::SwipeDown || event.type == TouchEventType::SwipeRight) {
            page_up();
            return true;
        }
        if (event.type == TouchEventType::LongPress) {
            g_keyboard_focus = false;
            int idx = hit_row(event.y);
            if (idx < 0) return false;
            g_focus = idx;
            begin_action_menu();
            return true;
        }
        if (event.type == TouchEventType::Tap) {
            g_keyboard_focus = false;
            const int tab=reading_navigation_layout::tab_at(event.x,event.y);
            if(tab>=0) {
                if(tab==0) {
                    launch_reading_home();
                } else if(tab==1) switch_to_bookshelf();
                else if(tab==3) switch_to_reading_settings();
                return true;
            }
            if(home_view()) {
                if(fl::transfer_add.contains(event.x,event.y)) {
                    m_transfer_menu=true; m_transfer_choice=0; redraw_all(); return true;
                }
                if(content_layout().sort_button.contains(event.x,event.y)) {
                    g_sort_ascending=!g_sort_ascending;
                    load_dir(g_path); redraw_all(); return true;
                }
            }
            if (hit_back(event.x, event.y)) {
                go_back_or_up();
                return true;
            }
            int idx = hit_row(event.y);

            if (idx >= 0) {
                g_focus = idx;
                open_focused();
                return true;
            }

        }
        return false;
    }

    bool on_navigation_key(int direction) override { return on_key(direction < 0 ? APP_KEY_UP : APP_KEY_DOWN); }
    bool on_key(int key) override {
        g_keyboard_focus = true;
        if (m_text_input.visible()) return m_text_input.handle_key(key);
        if(m_transfer_menu) {
            if(key==APP_KEY_BACK || key==APP_KEY_EXTRA) { m_transfer_menu=false; redraw_all(); }
            else if(key==APP_KEY_UP || key==APP_KEY_DOWN) {
                m_transfer_choice=(m_transfer_choice+(key==APP_KEY_UP?2:1))%3;
                redraw_all();
            } else if(key==APP_KEY_OK) select_transfer(m_transfer_choice);
            return true;
        }
        if (!m_notice.empty()) {
            if (key != APP_KEY_NONE) { m_notice.clear(); redraw_all(); }
            return true;
        }

        if (m_action_menu) {
            if (key == APP_KEY_BACK || key == APP_KEY_EXTRA) {
                m_action_menu = false;
                redraw_all();
            } else if (key == APP_KEY_UP) {
                int count = action_count();
                m_action_choice = (m_action_choice + count - 1) % count;
                redraw_all();
            } else if (key == APP_KEY_DOWN) {
                m_action_choice = (m_action_choice + 1) % action_count();
                redraw_all();
            } else if (key == APP_KEY_OK) {
                handle_action(m_action_choice);
            }
            return true;
        }
        if (m_confirm_delete) {
            if (key == APP_KEY_BACK || key == APP_KEY_EXTRA) {
                cancel_delete_confirm();
                return true;
            }
            if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
                m_delete_choice = 1 - m_delete_choice;
                redraw_all();
                return true;
            }
            if (key == APP_KEY_OK) {
                if (m_delete_choice == 1) confirm_delete();
                else cancel_delete_confirm();
                return true;
            }
            return true;
        }

        if (key == APP_KEY_BACK) {
            go_back_or_up();
            return true;
        }

        if (g_entries.empty()) return false;

        if (key == APP_KEY_EXTRA) {
            begin_action_menu();
            return true;
        }

        int vis_rows = visible_rows();

        if (key == APP_KEY_UP) {
            if (g_focus > 0) {
                g_focus--;
                if (g_focus < g_scroll) g_scroll = g_focus;
                redraw_all();
            }
            return true;
        }

        if (key == APP_KEY_DOWN) {
            if (g_focus + 1 < (int)g_entries.size()) {
                g_focus++;
                if (g_focus >= g_scroll + vis_rows) g_scroll = g_focus - vis_rows + 1;
                redraw_all();
            }
            return true;
        }

        if (key == APP_KEY_OK) {
            open_focused();
            return true;
        }

        return false;
    }

    void redraw_all() {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar();
        draw_list();
        if (m_transfer_menu) draw_transfer_menu();
        if (m_action_menu) draw_action_dialog();
        if (m_confirm_delete) draw_delete_dialog();
        if (!m_notice.empty()) draw_notice();
        M5.Display.display();
        M5.Display.endWrite();
    }
};

// ─── Placeholder (reuse from HomeActivity — defined there) ────────

void launch_file_browser(const char *start_path) {
    launch_file_browser(start_path, false);
}

void launch_file_browser(const char *start_path, bool initial_hd) {
    g_root = (start_path && start_path[0]) ? start_path : "/sdcard";
    g_path = g_root;
    load_dir(g_path);
    ActivityManager::instance().push(new FileBrowserActivity(initial_hd));
}

void switch_to_reading_files() {
    g_root = "/sdcard";
    g_path = g_root;
    load_dir(g_path);
    ActivityManager::instance().show_above_root(new FileBrowserActivity(true));
}
