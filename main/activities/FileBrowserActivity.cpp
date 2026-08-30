#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/MusicActivity.h"
#include "activities/PlaceholderActivity.h"
#include "activities/TextInputActivity.h"
#include "activities/recent_books.h"
#include "audio/audio_file_formats.h"
#include "reader_font.h"
#include "ui/SystemFont.h"
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
static const int TOP_BAR_H = 50;
static const int ROW_H     = 72;
static const int PAD       = 10;

struct FsEntry { std::string name; bool is_dir; };

static std::string g_path;
static std::string g_root;
static std::vector<FsEntry> g_entries;
static int g_scroll = 0;
static int g_focus  = 0;
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

static bool is_supported(const char *n, bool is_dir) {
    if (is_dir) return true;
    const char *ext = strrchr(n, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".epub") == 0 ||
            strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
            strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 ||
            strcasecmp(ext, ".ttf") == 0 ||
            strcasecmp(ext, ".otf") == 0 || strcasecmp(ext, ".bin") == 0 ||
            audio_file_formats::is_supported(n));
}

static bool is_font_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && (strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".otf") == 0 ||
                   strcasecmp(ext, ".bin") == 0);
}

static bool is_epub_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && strcasecmp(ext, ".epub") == 0;
}

static bool is_image_file(const char *n) {
    const char *ext = strrchr(n, '.');
    return ext && (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
                   strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0);
}

// ─── Drawing ───────────────────────────────────────────────────────

static void draw_topbar() {
    M5.Display.fillRect(0, 0, M5.Display.width(), TOP_BAR_H, TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

    // Path (truncated)
    std::string show = g_path;
    if (system_font::measure(show) > M5.Display.width() - 40) show = "..." + show.substr(show.size() > 35 ? show.size() - 35 : 0);
    show = system_font::ellipsize(show, M5.Display.width() - 40);
    system_font::draw_center(M5.Display, show, M5.Display.width() / 2, 3, TFT_BLACK);
    M5.Display.drawFastHLine(0, TOP_BAR_H - 1, M5.Display.width(), TFT_BLACK);
}

static void draw_list() {
    int w = M5.Display.width();
    int h = M5.Display.height();
    int y0 = TOP_BAR_H + 5;
    int vis_rows = (h - y0) / ROW_H;

    // Clamp scroll
    int max_scroll = std::max(0, (int)g_entries.size() - vis_rows);
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;
    if (g_focus >= (int)g_entries.size()) g_focus = g_entries.size() - 1;
    if (g_focus < 0) g_focus = 0;

    for (int i = 0; i < vis_rows; i++) {
        int idx = g_scroll + i;
        int ry = y0 + i * ROW_H;

        if (idx >= (int)g_entries.size()) {
            M5.Display.fillRect(0, ry, w, ROW_H, TFT_WHITE);
            continue;
        }

        bool sel = (idx == g_focus);
        const auto &e = g_entries[idx];

        M5.Display.fillRoundRect(PAD, ry, w - PAD - 12, ROW_H - 2, 6,
                                 sel ? TFT_BLACK : TFT_WHITE);
        M5.Display.drawRoundRect(PAD, ry, w - PAD - 12, ROW_H - 2, 6, TFT_BLACK);
        M5.Display.setTextColor(sel ? TFT_WHITE : TFT_BLACK,
                                sel ? TFT_BLACK : TFT_WHITE);

        std::string label = e.is_dir ? (e.name + "/") : e.name;
        label = system_font::ellipsize(label, w - PAD - 86);
        system_font::draw_left(M5.Display, label, PAD + 12, ry + 14, sel ? TFT_WHITE : TFT_BLACK);
    }

    // Page up/down buttons on right edge + scrollbar + count
    int bar_h = vis_rows * ROW_H;
    int btn_w = 50, btn_h = 50;
    int btn_up_y = y0 + 5;
    int btn_dn_y = y0 + bar_h - btn_h - 5;

    if (g_scroll > 0) {
        // Up arrow (filled triangle)
        M5.Display.fillTriangle(w - btn_w, btn_up_y,
                                w - 10, btn_up_y,
                                w - 10 - btn_w/2, btn_up_y + btn_h, TFT_BLACK);
    }
    int max_scroll2 = std::max(0, (int)g_entries.size() - vis_rows);
    if (g_scroll < max_scroll2) {
        // Down arrow
        M5.Display.fillTriangle(w - btn_w, btn_dn_y + btn_h,
                                w - 10, btn_dn_y + btn_h,
                                w - 10 - btn_w/2, btn_dn_y, TFT_BLACK);
    }

    // Count indicator
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d/%d", g_focus + 1, (int)g_entries.size());
    M5.Display.setTextColor(TFT_BLACK);
    system_font::draw_right(M5.Display, cnt, w - 16, M5.Display.height() - 52);
}

static void load_dir(const std::string &path) {
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
        e.is_dir = (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        if (!is_supported(ent->d_name, e.is_dir)) continue;
        g_entries.push_back(e);
    }
    closedir(d);

    // Sort: dirs first, then alphabetically
    std::sort(g_entries.begin(), g_entries.end(), [](auto &a, auto &b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
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
    int y0 = TOP_BAR_H + 5;
    if (ty < y0 || ty > M5.Display.height()) return -1;
    int idx = (ty - y0) / ROW_H + g_scroll;
    return (idx < (int)g_entries.size()) ? idx : -1;
}

static bool hit_back(int tx, int ty) {
    return (tx < 120 && ty < TOP_BAR_H);
}

// ─── Activity ──────────────────────────────────────────────────────

class FileBrowserActivity : public Activity {
    bool m_action_menu = false;
    int m_action_choice = 0; // 0=rename, 1=delete, 2=cancel
    bool m_confirm_delete = false;
    int m_delete_choice = 0; // 0=cancel, 1=delete
    std::string m_notice;

public:
    void on_create() override {
        ESP_LOGI(TAG, "create: %s", g_path.c_str());
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar();
        draw_list();
        M5.Display.display();
        M5.Display.endWrite();
    }

    void on_show() override {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar();
        draw_list();
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool hit_page_up(int tx, int ty) {
        int bar_h = (M5.Display.height() - TOP_BAR_H - 5) / ROW_H * ROW_H;
        int y0 = TOP_BAR_H + 5;
        return (tx >= M5.Display.width() - 55 && tx <= M5.Display.width()
                && ty >= y0 && ty <= y0 + bar_h / 2 && g_scroll > 0);
    }
    bool hit_page_dn(int tx, int ty) {
        int bar_h = (M5.Display.height() - TOP_BAR_H - 5) / ROW_H * ROW_H;
        int y0 = TOP_BAR_H + 5;
        int max_sc = std::max(0, (int)g_entries.size() - (int)(bar_h / ROW_H));
        return (tx >= M5.Display.width() - 55 && tx <= M5.Display.width()
                && ty >= y0 + bar_h / 2 && ty <= y0 + bar_h && g_scroll < max_sc);
    }

    std::string selected_path() const {
        if (g_focus < 0 || g_focus >= (int)g_entries.size()) return std::string();
        return g_path + "/" + g_entries[g_focus].name;
    }

    int visible_rows() const {
        return (M5.Display.height() - TOP_BAR_H - 5) / ROW_H;
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
        g_scroll += rows;
        g_focus = std::min((int)g_entries.size() - 1, g_focus + rows);
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
        } else if (is_epub_file(full.c_str())) {
            extern void launch_epub_reader(const char*);
            launch_epub_reader(full.c_str());
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

    void draw_action_dialog() {
        int w = M5.Display.width();
        int h = M5.Display.height();
        int box_w = std::min(w - 36, 650);
        int box_h = 250;
        int x = (w - box_w) / 2;
        int y = (h - box_h) / 2;
        M5.Display.fillRoundRect(x, y, box_w, box_h, 7, TFT_WHITE);
        M5.Display.drawRoundRect(x, y, box_w, box_h, 7, TFT_BLACK);
        std::string name = g_focus >= 0 && g_focus < (int)g_entries.size() ? g_entries[g_focus].name : "";
        system_font::draw_center(M5.Display, system_font::ellipsize(name, box_w - 40), w / 2, y + 28, TFT_BLACK);

        const char *labels[] = {"Rename", "Delete", "Cancel"};
        int gap = 12;
        int btn_w = (box_w - 40 - gap * 2) / 3;
        int btn_h = 66;
        int bx = x + 20;
        int by = y + box_h - btn_h - 24;
        for (int i = 0; i < 3; ++i) {
            bool selected = i == m_action_choice;
            M5.Display.fillRoundRect(bx, by, btn_w, btn_h, 6, selected ? TFT_BLACK : TFT_WHITE);
            M5.Display.drawRoundRect(bx, by, btn_w, btn_h, 6, TFT_BLACK);
            system_font::draw_center(M5.Display, labels[i], bx + btn_w / 2, by + 11,
                                     selected ? TFT_WHITE : TFT_BLACK);
            bx += btn_w + gap;
        }
    }

    int action_choice_at(int tx, int ty) const {
        int w = M5.Display.width();
        int h = M5.Display.height();
        int box_w = std::min(w - 36, 650);
        int box_h = 250;
        int x = (w - box_w) / 2;
        int y = (h - box_h) / 2;
        int gap = 12;
        int btn_w = (box_w - 40 - gap * 2) / 3;
        int btn_h = 66;
        int bx = x + 20;
        int by = y + box_h - btn_h - 24;
        if (ty < by || ty >= by + btn_h) return -1;
        for (int i = 0; i < 3; ++i) {
            if (tx >= bx && tx < bx + btn_w) return i;
            bx += btn_w + gap;
        }
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
            m_notice = "Invalid file name";
            return;
        }
        std::string new_path = g_path + "/" + new_name;
        if (new_path == old_path) return;
        struct stat st = {};
        bool case_only = strcasecmp(old_path.c_str(), new_path.c_str()) == 0;
        if (!case_only && stat(new_path.c_str(), &st) == 0) {
            m_notice = "A file with that name already exists";
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
            m_notice = "Rename failed";
            return;
        }

        recent_books_rename_path(old_path.c_str(), new_path.c_str());
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
        const FsEntry entry = g_entries[g_focus];
        const std::string old_path = selected_path();
        std::string stem = entry.name;
        std::string suffix;
        if (!entry.is_dir) {
            size_t dot = stem.find_last_of('.');
            if (dot != std::string::npos && dot > 0) {
                suffix = stem.substr(dot);
                stem.erase(dot);
            }
        }
        TextInputOptions options;
        options.title = "Rename";
        options.initial_text = stem;
        options.fixed_suffix = suffix;
        options.submit_label = "Rename";
        options.max_bytes = 255;
        options.start_in_chinese = true;
        launch_text_input(options, [this, old_path](const std::string &name) {
            rename_entry(old_path, name);
        });
    }

    void handle_action(int choice) {
        m_action_menu = false;
        m_action_choice = 0;
        if (choice == 0) begin_rename();
        else if (choice == 1) {
            if (g_focus >= 0 && g_focus < (int)g_entries.size() && g_entries[g_focus].is_dir) {
                m_notice = "Folder deletion is not supported";
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
        system_font::draw_center(M5.Display, "Tap to close", w / 2, y + 78, TFT_BLACK);
    }

    void draw_delete_dialog() {
        int w = M5.Display.width();
        int h = M5.Display.height();
        int box_w = std::min(w - 48, 620);
        int box_h = 260;
        int x = (w - box_w) / 2;
        int y = (h - box_h) / 2;

        M5.Display.fillRoundRect(x, y, box_w, box_h, 6, TFT_WHITE);
        M5.Display.drawRoundRect(x, y, box_w, box_h, 6, TFT_BLACK);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        system_font::draw_center(M5.Display, "Delete file?", w / 2, y + 18);

        std::string name = g_entries[g_focus].name;
        std::string label = system_font::ellipsize(ellipsize_middle(name, 32), box_w - 48);
        system_font::draw_center(M5.Display, label, w / 2, y + 82);

        int btn_h = 62;
        int gap = 18;
        int btn_w = (box_w - 48 - gap) / 2;
        int by = y + box_h - btn_h - 22;
        int bx0 = x + 24;
        int bx1 = bx0 + btn_w + gap;

        M5.Display.fillRoundRect(bx0, by, btn_w, btn_h, 6, m_delete_choice == 0 ? TFT_BLACK : TFT_WHITE);
        M5.Display.drawRoundRect(bx0, by, btn_w, btn_h, 6, TFT_BLACK);
        M5.Display.setTextColor(m_delete_choice == 0 ? TFT_WHITE : TFT_BLACK, m_delete_choice == 0 ? TFT_BLACK : TFT_WHITE);
        system_font::draw_center(M5.Display, "Cancel", bx0 + btn_w / 2, by + 9, m_delete_choice == 0 ? TFT_WHITE : TFT_BLACK);

        M5.Display.fillRoundRect(bx1, by, btn_w, btn_h, 6, m_delete_choice == 1 ? TFT_BLACK : TFT_WHITE);
        M5.Display.drawRoundRect(bx1, by, btn_w, btn_h, 6, TFT_BLACK);
        M5.Display.setTextColor(m_delete_choice == 1 ? TFT_WHITE : TFT_BLACK, m_delete_choice == 1 ? TFT_BLACK : TFT_WHITE);
        system_font::draw_center(M5.Display, "Delete", bx1 + btn_w / 2, by + 9, m_delete_choice == 1 ? TFT_WHITE : TFT_BLACK);
    }

    int delete_dialog_choice_at(int tx, int ty) const {
        int w = M5.Display.width();
        int h = M5.Display.height();
        int box_w = std::min(w - 48, 620);
        int box_h = 260;
        int x = (w - box_w) / 2;
        int y = (h - box_h) / 2;
        int btn_h = 62;
        int gap = 18;
        int btn_w = (box_w - 48 - gap) / 2;
        int by = y + box_h - btn_h - 22;
        int bx0 = x + 24;
        int bx1 = bx0 + btn_w + gap;

        if (ty < by || ty > by + btn_h) return -1;
        if (tx >= bx0 && tx <= bx0 + btn_w) return 0;
        if (tx >= bx1 && tx <= bx1 + btn_w) return 1;
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
            ESP_LOGI(TAG, "deleted: %s", path.c_str());
            load_dir(g_path);
            if (!g_entries.empty()) {
                g_focus = std::min(old_focus, (int)g_entries.size() - 1);
                int vis_rows = (M5.Display.height() - TOP_BAR_H - 5) / ROW_H;
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
        if (event.type == TouchEventType::Back) {
            if (!m_notice.empty()) { m_notice.clear(); redraw_all(); }
            else if (m_action_menu) { m_action_menu = false; redraw_all(); }
            else if (m_confirm_delete) cancel_delete_confirm();
            else go_back_or_up();
            return true;
        }

        if (!m_notice.empty()) {
            if (event.type == TouchEventType::Tap) { m_notice.clear(); redraw_all(); }
            return true;
        }

        if (m_action_menu) {
            if (event.type != TouchEventType::Tap) return true;
            int choice = action_choice_at(event.x, event.y);
            if (choice >= 0) handle_action(choice);
            return true;
        }

        if (m_confirm_delete) {
            if (event.type != TouchEventType::Tap) return true;
            int choice = delete_dialog_choice_at(event.x, event.y);
            if (choice >= 0) {
                m_delete_choice = choice;
                if (m_delete_choice == 1) confirm_delete();
                else cancel_delete_confirm();
            }
            return true;
        }

        if (event.type == TouchEventType::SwipeUp) {
            page_down();
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            page_up();
            return true;
        }
        if (event.type == TouchEventType::LongPress) {
            int idx = hit_row(event.y);
            if (idx < 0) return false;
            g_focus = idx;
            begin_action_menu();
            return true;
        }
        if (event.type == TouchEventType::Tap) {
            if (hit_back(event.x, event.y)) {
                go_back_or_up();
                return true;
            }
            if (hit_page_up(event.x, event.y)) {
                page_up();
                return true;
            }
            if (hit_page_dn(event.x, event.y)) {
                page_down();
                return true;
            }

            int idx = hit_row(event.y);

            if (idx >= 0) {
                if (idx != g_focus) {
                    g_focus = idx;
                    M5.Display.startWrite();
                    draw_list();
                    M5.Display.display();
                    M5.Display.endWrite();
                } else {
                    // Double-tap on same item → open
                    open_focused();
                    return true;
                }
                return true;
            }

        }
        return false;
    }

    bool on_key(int key) override {
        if (!m_notice.empty()) {
            if (key != APP_KEY_NONE) { m_notice.clear(); redraw_all(); }
            return true;
        }

        if (m_action_menu) {
            if (key == APP_KEY_BACK || key == APP_KEY_EXTRA) {
                m_action_menu = false;
                redraw_all();
            } else if (key == APP_KEY_UP) {
                m_action_choice = (m_action_choice + 2) % 3;
                redraw_all();
            } else if (key == APP_KEY_DOWN) {
                m_action_choice = (m_action_choice + 1) % 3;
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
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_topbar();
        draw_list();
        if (m_action_menu) draw_action_dialog();
        if (m_confirm_delete) draw_delete_dialog();
        if (!m_notice.empty()) draw_notice();
        M5.Display.display();
        M5.Display.endWrite();
    }
};

// ─── Placeholder (reuse from HomeActivity — defined there) ────────

void launch_file_browser(const char *start_path) {
    g_root = (start_path && start_path[0]) ? start_path : "/sdcard";
    g_path = g_root;
    load_dir(g_path);
    ActivityManager::instance().push(new FileBrowserActivity());
}
