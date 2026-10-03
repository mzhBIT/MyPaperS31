#pragma once

#include "activities/Activity.h"
#include "ui/ReaderProfile.h"

#include <string>
#include "ui/script_conversion.h"
#include "ui/reader_writing_mode.h"

struct ReaderOverlayChapters {
    void *context = nullptr;
    int (*count)(void *context) = nullptr;
    int (*current)(void *context) = nullptr;
    std::string (*title)(void *context, int index) = nullptr;
    int (*depth)(void *context, int index) = nullptr;
    bool (*activate)(void *context, int index) = nullptr;
    int (*progress)(void *context) = nullptr;                  // 0..100 percent.
    bool (*seek)(void *context, int percent) = nullptr;
};

class ReaderOverlay {
public:
    enum class Tab {
        Settings,
        Beauty,
        Chapters,
    };

    enum class Action {
        None,
        Close,
        OpenFont,
        OpenBackground,
        OpenBookCover,
        ChapterOpened,
    };

    void set_profile(ReaderProfile profile) { m_profile = profile; }
    void set_chapters(const ReaderOverlayChapters &chapters);
    void set_chapters_enabled(bool enabled);
    void set_weread_mode(bool enabled);
    void set_book_path(const char *path);
    script_conversion::Mode script_mode() const { return m_script_mode; }
    reader_writing_mode::Mode writing_mode() const { return m_writing_mode; }
    void open(Tab tab);
    void close();
    // Applies the last progress-bar selection, if any. The selection remains
    // a preview until the owner explicitly closes the overlay.
    bool commit_pending_seek();

    bool visible() const { return m_visible; }
    bool settings_dirty() const { return m_settings_dirty; }
    void clear_settings_dirty() { m_settings_dirty = false; }

    Action on_key(int key);
    Action on_touch(const TouchEvent &event);

    // Paints into the current M5.Display transaction.
    void draw();
    // Repaints just the floating panel using the fast waveform.
    void redraw();

private:
    static constexpr int SETTING_COUNT = 9;
    static constexpr int BEAUTY_COUNT = 8;
    static constexpr int PROGRESS_AREA_H = 118;

    struct Geometry {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int tab_h = 0;
        int body_y = 0;
        int body_h = 0;
        int tab_y = 0;
    };

    Geometry geometry() const;
    void select_tab(Tab tab);
    void adjust_setting(int delta);
    int beauty_base_index(int index) const;
    bool is_public_comments_row(int index) const;
    void move_setting_focus(int delta);
    int active_setting_count() const;
    void move_chapter_focus(int delta);
    int chapter_count() const;
    int visible_chapter_rows() const;
    void clamp_chapter_scroll();
    int hit_setting(int x, int y, int *direction) const;
    int hit_chapter(int y) const;
    int hit_progress(int x, int y) const;
    Action activate_setting();
    Action activate_chapter();
    void draw_settings(const Geometry &g);
    void draw_beauty(const Geometry &g);
    void draw_chapters(const Geometry &g);
    void draw_tabs(const Geometry &g);

    ReaderProfile m_profile = ReaderProfile::Shared;
    ReaderOverlayChapters m_chapters;
    bool m_chapters_enabled = true;
    bool m_weread_mode = false;
    bool m_visible = false;
    bool m_settings_dirty = false;
    std::string m_book_path;
    script_conversion::Mode m_script_mode = script_conversion::Mode::Original;
    reader_writing_mode::Mode m_writing_mode = reader_writing_mode::Mode::Auto;
    bool m_script_load_failed = false;
    Tab m_tab = Tab::Settings;
    int m_setting_focus = 0;
    int m_chapter_focus = 0;
    int m_chapter_scroll = 0;
    int m_pending_progress = -1;
};
