#pragma once

#include "activities/Activity.h"

#include <string>

struct ReaderOverlayChapters {
    void *context = nullptr;
    int (*count)(void *context) = nullptr;
    int (*current)(void *context) = nullptr;
    std::string (*title)(void *context, int index) = nullptr;
    int (*depth)(void *context, int index) = nullptr;
    bool (*activate)(void *context, int index) = nullptr;
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
        ChapterOpened,
    };

    void set_chapters(const ReaderOverlayChapters &chapters);
    void set_chapters_enabled(bool enabled);
    void open(Tab tab);
    void close();

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
    static constexpr int SETTING_COUNT = 7;
    static constexpr int BEAUTY_COUNT = 6;

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
    void move_setting_focus(int delta);
    int active_setting_count() const;
    void move_chapter_focus(int delta);
    int chapter_count() const;
    int visible_chapter_rows() const;
    void clamp_chapter_scroll();
    int hit_setting(int x, int y, int *direction) const;
    int hit_chapter(int y) const;
    Action activate_setting();
    Action activate_chapter();
    void draw_settings(const Geometry &g);
    void draw_beauty(const Geometry &g);
    void draw_chapters(const Geometry &g);
    void draw_tabs(const Geometry &g);

    ReaderOverlayChapters m_chapters;
    bool m_chapters_enabled = true;
    bool m_visible = false;
    bool m_settings_dirty = false;
    Tab m_tab = Tab::Settings;
    int m_setting_focus = 0;
    int m_chapter_focus = 0;
    int m_chapter_scroll = 0;
};
