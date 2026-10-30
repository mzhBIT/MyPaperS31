#include "activities/MusicActivity.h"
#include "ui/ListFocus.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "audio/audio_file_formats.h"
#include "audio/audio_radio.h"
#include "audio/usb_audio_output.h"
#include "audio/embedded_cover.h"
#include "audio/embedded_lyrics.h"
#include "renderer/CoverThumbnail.h"
#include "renderer/P4JpegDecoder.h"
#include "ui/SystemFont.h"
#include "ui/AppStatusBar.h"
#include "ui/MusicLyricLayout.h"
#include "ui/UiLocale.h"
#include "ui/music_icons_1bpp.h"

#include <M5Unified.h>
#include <dirent.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <sys/stat.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

namespace {

constexpr const char *TAG = "MUSIC";
constexpr const char *MUSIC_ROOT = "/sdcard";
constexpr int MAX_SCAN_DEPTH = 16;

constexpr int DESIGN_W = 684;
constexpr int DESIGN_H = 1216;
constexpr int TITLE_X = 73;
constexpr int TITLE_Y = 74;
constexpr int COVER_X = 85;
constexpr int COVER_Y = 158;
constexpr int COVER_W = 515;
constexpr int COVER_H = 708;
constexpr int PROGRESS_X0 = 84;
constexpr int PROGRESS_X1 = 599;
constexpr int PROGRESS_Y = 903;
constexpr int PROGRESS_TEXT_Y = 914;
constexpr int64_t PROGRESS_UPDATE_US = 1000000;
constexpr int MAIN_CONTROL_Y = 996;
constexpr int BOTTOM_CONTROL_Y = 1135;
constexpr int LIST_ROW_H = 62;
constexpr int LIST_INSET = 8;
constexpr int LIST_FOOTER_H = 62;
constexpr int LYRICS_TOP = COVER_Y + 18;
constexpr int LYRICS_BOTTOM = COVER_Y + COVER_H - 52;
constexpr int ALBUM_ART_SIZE = 430;
constexpr int ALBUM_ART_X = COVER_X + (COVER_W - ALBUM_ART_SIZE) / 2;
constexpr int ALBUM_ART_Y = COVER_Y + 48;
constexpr int VIEW_INDICATOR_Y = COVER_Y + COVER_H - 14;
constexpr int VOLUME_TOAST_H = 58;
// Place the transient value beside the volume-up icon, vertically aligned
// with the bottom controls and flush toward the screen's right edge.
constexpr int VOLUME_TOAST_X = DESIGN_W - 72;
constexpr int VOLUME_TOAST_Y = BOTTOM_CONTROL_Y - VOLUME_TOAST_H / 2;
constexpr int VOLUME_TOAST_W = DESIGN_W - VOLUME_TOAST_X;
constexpr int64_t VOLUME_TOAST_US = 1000000;

struct Track {
    std::string path;
    std::string label;
};

enum class ContentView : uint8_t {
    Cover = 0,
    Lyrics,
    List,
};

enum class PlayMode : uint8_t {
    Sequence = 0,
    Shuffle,
    RepeatOne,
};

int design_x(int value)
{
    return static_cast<int>((static_cast<int64_t>(value) * M5.Display.width()) / DESIGN_W);
}

int design_y(int value)
{
    return static_cast<int>((static_cast<int64_t>(value) * M5.Display.height()) / DESIGN_H);
}

std::string format_playback_time(uint32_t time_ms, bool known = true)
{
    if (!known) return "--:--";
    uint32_t total_seconds = time_ms / 1000;
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds / 60) % 60;
    uint32_t seconds = total_seconds % 60;
    char text[24] = {};
    if (hours > 0) {
        snprintf(text, sizeof(text), "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                 hours, minutes, seconds);
    } else {
        snprintf(text, sizeof(text), "%" PRIu32 ":%02" PRIu32, minutes, seconds);
    }
    return text;
}

bool skip_name(const char *name)
{
    if (name == nullptr || name[0] == '\0') return true;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return true;
    if (name[0] == '.') return true;
    return strcasecmp(name, "System Volume Information") == 0 ||
           strcasecmp(name, "$RECYCLE.BIN") == 0;
}

std::string relative_label(const std::string &path)
{
    constexpr size_t root_len = 7;
    if (path.size() > root_len && path.compare(0, root_len, MUSIC_ROOT) == 0) {
        size_t start = path[root_len] == '/' ? root_len + 1 : root_len;
        return path.substr(start);
    }
    return path;
}

std::string base_name(const std::string &path)
{
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string track_title(const std::string &path)
{
    std::string title = base_name(path);
    size_t extension = title.find_last_of('.');
    if (extension != std::string::npos && extension > 0) title.erase(extension);
    return title;
}

void scan_music_dir(const std::string &path, int depth, std::vector<Track> &tracks)
{
    if (depth > MAX_SCAN_DEPTH) {
        ESP_LOGW(TAG, "Skip directory deeper than %d: %s", MAX_SCAN_DEPTH, path.c_str());
        return;
    }

    DIR *dir = opendir(path.c_str());
    if (dir == nullptr) {
        ESP_LOGW(TAG, "Cannot open directory: %s", path.c_str());
        return;
    }

    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (skip_name(entry->d_name)) continue;

        std::string full_path = path;
        if (full_path.empty() || full_path.back() != '/') full_path.push_back('/');
        full_path += entry->d_name;

        struct stat st = {};
        if (stat(full_path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_music_dir(full_path, depth + 1, tracks);
        } else if (S_ISREG(st.st_mode) && audio_file_formats::is_supported(entry->d_name)) {
            tracks.push_back({full_path, relative_label(full_path)});
        }
    }
    closedir(dir);
}

class MusicActivity final : public Activity {
public:
    explicit MusicActivity(const char *start_path = nullptr)
        : _initial_path(start_path ? start_path : "")
    {
    }

    ~MusicActivity() override
    {
        audio_radio_deinit();
    }

#if CONFIG_IDF_TARGET_ESP32S31
    bool prepare_standby() override
    {
        if (audio_radio_stop()!=ESP_OK) return false;
        audio_radio_deinit();
        return usb_audio_output_is_quiescent();
    }
#endif

    void on_create() override
    {
        _observed_volume = audio_radio_get_volume();
        draw_message(ui_locale::text(ui_locale::TextId::ScanningAudioFiles));
        scan_music_dir(MUSIC_ROOT, 0, _tracks);
        std::sort(_tracks.begin(), _tracks.end(), [](const Track &a, const Track &b) {
            int cmp = strcasecmp(a.label.c_str(), b.label.c_str());
            return cmp == 0 ? a.path < b.path : cmp < 0;
        });
        ESP_LOGI(TAG, "Found %u supported audio files", (unsigned)_tracks.size());
        if (!_initial_path.empty()) {
            auto selected = std::find_if(_tracks.begin(), _tracks.end(), [this](const Track &track) {
                return track.path == _initial_path ||
                       strcasecmp(track.path.c_str(), _initial_path.c_str()) == 0;
            });
            if (selected != _tracks.end()) {
                int index = (int)std::distance(_tracks.begin(), selected);
                ESP_LOGI(TAG, "Open requested track %d/%u: %s", index + 1,
                         (unsigned)_tracks.size(), _initial_path.c_str());
                _initial_path.clear();
                play_index(index);
                return;
            }
            ESP_LOGW(TAG, "Requested track was not found after scan: %s",
                     _initial_path.c_str());
            _initial_path.clear();
        }
        if (_tracks.empty()) _status = ui_locale::text(ui_locale::TextId::NoPlayableAudioOnSd);
        draw_all();
    }

    void loop() override
    {
        audio_radio_service_output();
        audio_playback_state_t state = audio_radio_get_state();
        if (state == AUDIO_PLAYBACK_FINISHED && !_terminal_handled && _current >= 0) {
            _terminal_handled = true;
            play_next(true);
            return;
        } else if (state == AUDIO_PLAYBACK_ERROR && !_terminal_handled) {
            _terminal_handled = true;
            _status = ui_locale::text(ui_locale::TextId::PlaybackFailed);
            draw_all();
            return;
        }
        const int volume = audio_radio_get_volume();
        if (volume != _observed_volume) {
            _observed_volume = volume;
            _volume_toast_value = volume;
            _volume_toast_pending = true;
        }
        if (update_volume_toast()) return;
        update_progress_if_due();
        update_controls_if_state_changed();
        if (!_refresh_frozen) _status_bar.refresh(ui_locale::text(ui_locale::TextId::Music));
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::SwipeLeft ||
            event.type == TouchEventType::SwipeRight) {
            int start_y = event.start_y != 0 ? event.start_y : event.y;
            if (start_y >= design_y(COVER_Y) && start_y <= design_y(COVER_Y + COVER_H) &&
                _current >= 0) {
                switch_view(event.type == TouchEventType::SwipeLeft ? 1 : -1);
                return true;
            }
            return false;
        }
        if (event.type == TouchEventType::SwipeUp) {
            if (_view == ContentView::List) page_down();
            return _view == ContentView::List;
        }
        if (event.type == TouchEventType::SwipeDown) {
            if (_view == ContentView::List) page_up();
            return _view == ContentView::List;
        }
        if (event.type != TouchEventType::Tap) return false;

        if (app_status_bar_layout::back_hit(event.x,event.y)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (app_status_bar_layout::audio_output(M5.Display.width()).contains(event.x,event.y)) {
            cycle_audio_output();
            return true;
        }

        if (hit_center(event.x, event.y, 207, MAIN_CONTROL_Y, 62, 55)) {
            play_previous();
            return true;
        }
        if (hit_center(event.x, event.y, 342, MAIN_CONTROL_Y, 62, 55)) {
            toggle_pause();
            return true;
        }
        if (hit_center(event.x, event.y, 479, MAIN_CONTROL_Y, 62, 55)) {
            play_next();
            return true;
        }
        if (hit_center(event.x, event.y, 91, BOTTOM_CONTROL_Y, 52, 46)) {
            cycle_mode();
            return true;
        }
        if (hit_center(event.x, event.y, 207, BOTTOM_CONTROL_Y, 52, 46)) {
            toggle_refresh_freeze();
            return true;
        }
        if (hit_center(event.x, event.y, 342, BOTTOM_CONTROL_Y, 52, 46)) {
            if (_view == ContentView::List && _current < 0) return true;
            _view = _view == ContentView::List ? ContentView::Cover : ContentView::List;
            ensure_focus_visible();
            draw_all();
            return true;
        }
        if (hit_center(event.x, event.y, 474, BOTTOM_CONTROL_Y, 52, 46)) {
            adjust_volume(-10);
            return true;
        }
        if (hit_center(event.x, event.y, 579, BOTTOM_CONTROL_Y, 52, 46)) {
            adjust_volume(10);
            return true;
        }

        int index = hit_track(event.x, event.y);
        if (index >= 0) {
            _focus = index;
            if (_current == index) toggle_pause();
            else play_index(index);
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_EXTRA) {
            cycle_mode();
            return true;
        }
        if (_tracks.empty()) return false;
        if (key == APP_KEY_UP && _focus > 0) {
            _view = ContentView::List;
            --_focus;
            ensure_focus_visible();
            draw_all();
            return true;
        }
        if (key == APP_KEY_DOWN && _focus + 1 < (int)_tracks.size()) {
            _view = ContentView::List;
            ++_focus;
            ensure_focus_visible();
            draw_all();
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_view == ContentView::List && _current != _focus) play_index(_focus);
            else toggle_pause();
            return true;
        }
        return false;
    }

    bool on_volume_key(int delta) override {
        adjust_volume(delta);
        return true;
    }
private:
    app_status_bar::State _status_bar;
    std::string _initial_path;
    std::vector<Track> _tracks;
    std::vector<int> _shuffle_order;
    std::vector<embedded_lyrics::Line> _lyrics;
    int _shuffle_pos = -1;
    int _focus = 0;
    int _scroll = 0;
    int _current = -1;
    PlayMode _play_mode = PlayMode::Sequence;
    ContentView _view = ContentView::List;
    bool _terminal_handled = true;
    bool _refresh_frozen = false;
    uint32_t _progress_permille = 0;
    uint32_t _elapsed_ms = 0;
    uint32_t _duration_ms = 0;
    int64_t _last_progress_update_us = 0;
    int _last_lyric_index = -2;
    int _last_lyric_window = -1;
    int _lyric_wrap_width = 0;
    std::vector<std::string> _lyric_lines;
    std::vector<int> _lyric_starts;
    int _volume_toast_value = 0;
    int _observed_volume = -1;
    int64_t _volume_toast_hide_us = 0;
    bool _volume_toast_pending = false;
    bool _volume_toast_drawn = false;
    std::string _cover_thumbnail_path;
    std::string _status;
    // State the control row was last drawn with. Audio output changes flip the
    // play/pause glyph without any touch, so the row needs to notice on its own.
    audio_playback_state_t _controls_state = AUDIO_PLAYBACK_IDLE;

    bool hit_center(int x, int y, int center_x, int center_y, int half_w, int half_h) const
    {
        return x >= design_x(center_x - half_w) && x <= design_x(center_x + half_w) &&
               y >= design_y(center_y - half_h) && y <= design_y(center_y + half_h);
    }

    void switch_view(int direction)
    {
        int view = static_cast<int>(_view);
        view = (view + direction + 3) % 3;
        _view = static_cast<ContentView>(view);
        ensure_focus_visible();
        draw_all();
    }

    int list_top() const
    {
        return design_y(COVER_Y + LIST_INSET);
    }

    int list_row_height() const
    {
        return std::max(1, design_y(LIST_ROW_H));
    }

    int visible_rows() const
    {
        int usable = design_y(COVER_H - LIST_INSET * 2 - LIST_FOOTER_H);
        return std::max(1, usable / list_row_height());
    }

    void ensure_focus_visible()
    {
        int rows = visible_rows();
        if (_focus < _scroll) _scroll = _focus;
        if (_focus >= _scroll + rows) _scroll = _focus - rows + 1;
        int max_scroll = std::max(0, (int)_tracks.size() - rows);
        _scroll = std::max(0, std::min(_scroll, max_scroll));
    }

    int hit_track(int x, int y) const
    {
        if (_view != ContentView::List ||
            x < design_x(COVER_X) || x > design_x(COVER_X + COVER_W)) return -1;
        if (y < list_top()) return -1;
        int row = (y - list_top()) / list_row_height();
        if (row < 0 || row >= visible_rows()) return -1;
        int index = _scroll + row;
        return index >= 0 && index < (int)_tracks.size() ? index : -1;
    }

    void page_up()
    {
        int rows = visible_rows();
        _focus = std::max(0, _focus - rows);
        _scroll = std::max(0, _scroll - rows);
        draw_all();
    }

    void page_down()
    {
        if (_tracks.empty()) return;
        int rows = visible_rows();
        _focus = std::min((int)_tracks.size() - 1, _focus + rows);
        _scroll = std::min(std::max(0, (int)_tracks.size() - rows), _scroll + rows);
        draw_all();
    }

    void rebuild_shuffle(int first)
    {
        _shuffle_order.resize(_tracks.size());
        for (int i = 0; i < (int)_shuffle_order.size(); ++i) _shuffle_order[i] = i;
        for (int i = (int)_shuffle_order.size() - 1; i > 0; --i) {
            int j = (int)(esp_random() % (uint32_t)(i + 1));
            std::swap(_shuffle_order[i], _shuffle_order[j]);
        }
        auto it = std::find(_shuffle_order.begin(), _shuffle_order.end(), first);
        if (it != _shuffle_order.end() && !_shuffle_order.empty()) {
            std::iter_swap(_shuffle_order.begin(), it);
            _shuffle_pos = 0;
        } else {
            _shuffle_pos = -1;
        }
    }

    void cycle_mode()
    {
        int next = (static_cast<int>(_play_mode) + 1) % 3;
        _play_mode = static_cast<PlayMode>(next);
        if (_play_mode == PlayMode::Shuffle && !_tracks.empty()) {
            rebuild_shuffle(_current >= 0 ? _current : _focus);
        } else {
            _shuffle_order.clear();
            _shuffle_pos = -1;
        }
        // The selected mode is already visible on the active control icon.
        _status.clear();
        draw_all();
    }

    void prepare_cover(const std::string &audio_path)
    {
        _cover_thumbnail_path.clear();
        std::string source_path;
        if (!embedded_cover::find_or_extract(audio_path.c_str(), &source_path)) return;
        std::string thumbnail_path = embedded_cover::square_thumbnail_path(audio_path.c_str());
        if (!thumbnail_path.empty() &&
            cover_thumbnail::ensure_square(source_path, thumbnail_path)) {
            _cover_thumbnail_path = thumbnail_path;
        } else {
            ESP_LOGW(TAG, "Cover thumbnail failed source=%s", source_path.c_str());
        }
    }

    void play_index(int index, bool keep_shuffle_order = false)
    {
        if (index < 0 || index >= (int)_tracks.size()) return;
        _focus = index;
        ensure_focus_visible();
        _progress_permille = 0;
        _elapsed_ms = 0;
        _duration_ms = 0;
        _last_progress_update_us = 0;
        _last_lyric_index = -2;
        _lyrics.clear();
        _lyric_lines.clear();
        _lyric_starts.clear();
        _lyric_wrap_width = 0;
        _last_lyric_window = -1;
        _cover_thumbnail_path.clear();
        _status = ui_locale::text(ui_locale::TextId::OpeningAudio);

        audio_playback_state_t previous_state = audio_radio_get_state();
        if (previous_state == AUDIO_PLAYBACK_PLAYING ||
            previous_state == AUDIO_PLAYBACK_PAUSED) {
            esp_err_t stop_ret = audio_radio_stop();
            if (stop_ret != ESP_OK) {
                ESP_LOGW(TAG, "Stop before track preparation failed: %s",
                         esp_err_to_name(stop_ret));
            }
        }
        // Keep SD-heavy metadata and image preparation outside active audio
        // playback. This avoids contending with the decoder while the cover
        // thumbnail is generated for the first time.
        embedded_lyrics::load(_tracks[index].path.c_str(), &_lyrics);
        prepare_cover(_tracks[index].path);

        esp_err_t ret = audio_radio_play_file(_tracks[index].path.c_str());
        if (ret == ESP_OK) {
            _current = index;
            // Selecting a song from the list opens playback, but track changes
            // must preserve the user's cover/lyrics view (including auto-next).
            if (_view == ContentView::List) _view = ContentView::Cover;
            _terminal_handled = false;
            _status.clear();
            if (_play_mode == PlayMode::Shuffle && !keep_shuffle_order) {
                rebuild_shuffle(index);
            }
            ESP_LOGI(TAG, "Playing %d/%u: %s", index + 1,
                     (unsigned)_tracks.size(), _tracks[index].path.c_str());
        } else {
            _lyrics.clear();
            _cover_thumbnail_path.clear();
            _terminal_handled = true;
            char message[64] = {};
            snprintf(message, sizeof(message), "%s%s",
                     ui_locale::text(ui_locale::TextId::PlayFailedPrefix), esp_err_to_name(ret));
            _status = message;
            ESP_LOGE(TAG, "Failed to play %s: %s", _tracks[index].path.c_str(),
                     esp_err_to_name(ret));
        }
        draw_all();
    }

    void play_next(bool automatic = false)
    {
        if (_tracks.empty()) return;
        if (automatic && _play_mode == PlayMode::RepeatOne) {
            restart_current();
            return;
        }
        int next = 0;
        if (_play_mode == PlayMode::Shuffle) {
            if (_shuffle_order.size() != _tracks.size() || _shuffle_pos < 0) {
                rebuild_shuffle(_current >= 0 ? _current : _focus);
            }
            if (_shuffle_order.size() == 1) {
                next = _shuffle_order[0];
            } else if (_shuffle_pos + 1 < (int)_shuffle_order.size()) {
                next = _shuffle_order[++_shuffle_pos];
            } else {
                rebuild_shuffle(_current >= 0 ? _current : _focus);
                _shuffle_pos = std::min(1, (int)_shuffle_order.size() - 1);
                next = _shuffle_order[_shuffle_pos];
            }
        } else {
            next = _current >= 0 ? (_current + 1) % (int)_tracks.size() : _focus;
        }
        play_index(next, true);
    }

    void restart_current()
    {
        if (_current < 0 || _current >= static_cast<int>(_tracks.size())) {
            play_index(_focus, true);
            return;
        }
        _progress_permille = 0;
        _elapsed_ms = 0;
        _duration_ms = 0;
        _last_progress_update_us = 0;
        _last_lyric_index = -2;
        esp_err_t ret = audio_radio_play_file(_tracks[_current].path.c_str());
        if (ret != ESP_OK) {
            _terminal_handled = true;
            _status = ui_locale::text(ui_locale::TextId::RepeatOneFailed);
            draw_all();
            return;
        }
        _terminal_handled = false;
        _status.clear();

        if (_refresh_frozen) {
            _last_progress_update_us = esp_timer_get_time();
            _last_lyric_index = current_lyric_index();
            return;
        }

        // The cover, title and controls are unchanged. Reset only the time
        // and visible lyric area so repeat-one does not trigger a full-screen
        // refresh or regenerate cover/lyric metadata at every loop.
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_progress_section(0);
        if (_view == ContentView::Cover) draw_current_lyric_section();
        else if (_view == ContentView::Lyrics) draw_lyrics_section();
        M5.Display.display();
        M5.Display.endWrite();
        _last_progress_update_us = esp_timer_get_time();
        _last_lyric_index = current_lyric_index();
    }

    void play_previous()
    {
        if (_tracks.empty()) return;
        int previous = 0;
        if (_play_mode == PlayMode::Shuffle) {
            if (_shuffle_order.size() != _tracks.size() || _shuffle_pos < 0) {
                rebuild_shuffle(_current >= 0 ? _current : _focus);
            }
            if (_shuffle_order.size() == 1) {
                previous = _shuffle_order[0];
            } else {
                _shuffle_pos = _shuffle_pos > 0 ? _shuffle_pos - 1 : (int)_shuffle_order.size() - 1;
                previous = _shuffle_order[_shuffle_pos];
            }
        } else {
            previous = _current >= 0
                           ? (_current - 1 + (int)_tracks.size()) % (int)_tracks.size()
                           : _focus;
        }
        play_index(previous, true);
    }

    void toggle_pause()
    {
        if (_tracks.empty()) return;
        if (_current < 0) {
            play_index(_focus);
            return;
        }

        audio_playback_state_t state = audio_radio_get_state();
        esp_err_t ret = ESP_OK;
        if (state == AUDIO_PLAYBACK_PLAYING) {
            ret = audio_radio_pause();
        } else if (state == AUDIO_PLAYBACK_PAUSED) {
            ret = audio_radio_resume();
        } else {
            play_index(_current);
            return;
        }
        if (ret != ESP_OK) {
            char message[64] = {};
            snprintf(message, sizeof(message), "%s%s",
                     ui_locale::text(ui_locale::TextId::ControlFailedPrefix), esp_err_to_name(ret));
            _status = message;
        } else {
            _status.clear();
        }
        draw_all();
    }

    void adjust_volume(int delta)
    {
        int volume = std::max(0, std::min(100, audio_radio_get_volume() + delta));
        esp_err_t ret = audio_radio_set_volume(volume);
        if (ret == ESP_OK) {
            _status.clear();
            _observed_volume = volume;
            _volume_toast_value = volume;
            _volume_toast_pending = true;
            update_volume_toast();
        } else {
            char message[64] = {};
            snprintf(message, sizeof(message), "%s%s",
                     ui_locale::text(ui_locale::TextId::VolumeFailedPrefix), esp_err_to_name(ret));
            _status = message;
            draw_all();
        }
    }

    void draw_volume_toast(bool visible)
    {
        int x = design_x(VOLUME_TOAST_X);
        int y = design_y(VOLUME_TOAST_Y);
        int width = std::max(1, design_x(VOLUME_TOAST_W));
        int height = std::max(1, design_y(VOLUME_TOAST_H));
        M5.Display.fillRect(x, y, width, height, TFT_WHITE);
        if (!visible) return;

        char value[16] = {};
        snprintf(value, sizeof(value), "%d", _volume_toast_value);
        system_font::draw_center(
            M5.Display, value, x + width / 2,
            y + std::max(0, (height - system_font::line_height()) / 2), TFT_BLACK);
    }

    bool update_volume_toast()
    {
        int64_t now = esp_timer_get_time();
        bool show = _volume_toast_pending;
        bool hide = _volume_toast_drawn && now >= _volume_toast_hide_us;
        if (!show && !hide) return false;
        if (M5.Display.displayBusy()) return false;

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_volume_toast(show);
        M5.Display.display();
        M5.Display.endWrite();

        if (show) {
            _volume_toast_pending = false;
            _volume_toast_drawn = true;
            _volume_toast_hide_us = now + VOLUME_TOAST_US;
        } else {
            _volume_toast_drawn = false;
            _volume_toast_hide_us = 0;
        }
        return true;
    }

    const char *state_text() const
    {
        if (!_status.empty()) return _status.c_str();
        switch (audio_radio_get_state()) {
            case AUDIO_PLAYBACK_PLAYING: return ui_locale::text(ui_locale::TextId::Playing);
            case AUDIO_PLAYBACK_PAUSED: return ui_locale::text(ui_locale::TextId::Paused);
            case AUDIO_PLAYBACK_FINISHED: return ui_locale::text(ui_locale::TextId::PlaybackFinished);
            case AUDIO_PLAYBACK_ERROR: return ui_locale::text(ui_locale::TextId::PlaybackFailed);
            case AUDIO_PLAYBACK_STOPPED: return ui_locale::text(ui_locale::TextId::Stopped);
            default: return ui_locale::text(_tracks.empty() ? ui_locale::TextId::NoPlayableSongs
                                                             : ui_locale::TextId::SelectSong);
        }
    }

    uint32_t read_progress()
    {
        if (_current < 0) return 0;

        uint32_t elapsed_ms = 0;
        uint32_t duration_ms = 0;
        if (audio_radio_get_file_timing(&elapsed_ms, &duration_ms) == ESP_OK) {
            _elapsed_ms = std::max(_elapsed_ms, elapsed_ms);
            if (duration_ms > 0) _duration_ms = duration_ms;
            if (_duration_ms > 0) {
                uint32_t timed_progress = _elapsed_ms >= _duration_ms
                                              ? 1000
                                              : (uint32_t)((uint64_t)_elapsed_ms * 1000u /
                                                           _duration_ms);
                _progress_permille = std::max(_progress_permille, timed_progress);
                return _progress_permille;
            }
        }

        uint32_t progress = _progress_permille;
        if (audio_radio_get_file_progress(&progress) == ESP_OK) {
            _progress_permille = std::max(_progress_permille,
                                          std::min<uint32_t>(progress, 1000));
        }
        return _progress_permille;
    }

    int current_lyric_index() const
    {
        if (_lyrics.empty()) return -1;
        auto next = std::upper_bound(
            _lyrics.begin(), _lyrics.end(), _elapsed_ms,
            [](uint32_t time_ms, const embedded_lyrics::Line &line) {
                return time_ms < line.time_ms;
            });
        return next == _lyrics.begin() ? -1 : static_cast<int>(next - _lyrics.begin() - 1);
    }

    void draw_view_indicator()
    {
        int center_x = design_x(COVER_X + COVER_W / 2);
        int y = design_y(VIEW_INDICATOR_Y);
        int gap = std::max(8, design_x(18));
        for (int i = 0; i < 3; ++i) {
            int radius = i == static_cast<int>(_view) ? std::max(3, design_x(5))
                                                       : std::max(2, design_x(3));
            if (i == static_cast<int>(_view)) {
                M5.Display.fillCircle(center_x + (i - 1) * gap, y, radius, TFT_BLACK);
            } else {
                M5.Display.drawCircle(center_x + (i - 1) * gap, y, radius, TFT_BLACK);
            }
        }
    }

    struct LyricArea { int left, top, right, bottom, inset; };

    LyricArea lyric_area() const
    {
        if (_view == ContentView::Cover) {
            const int radius = std::max(3, design_x(5));
            return {design_x(COVER_X + 18),
                    design_y(ALBUM_ART_Y) + std::max(1, design_y(ALBUM_ART_SIZE)) +
                        std::max(6, design_y(12)),
                    design_x(COVER_X + COVER_W - 18),
                    design_y(VIEW_INDICATOR_Y) - radius - std::max(4, design_y(6)),
                    design_x(10)};
        }
        return {design_x(COVER_X + 10), design_y(LYRICS_TOP),
                design_x(COVER_X + COVER_W - 10), design_y(LYRICS_BOTTOM),
                design_x(18)};
    }

    void prepare_lyric_lines(int width)
    {
        if (_lyric_wrap_width == width && _lyric_starts.size() == _lyrics.size() + 1) return;
        _lyric_wrap_width = width;
        _lyric_lines.clear();
        _lyric_starts.clear();
        for (const auto &lyric : _lyrics) {
            _lyric_starts.push_back(static_cast<int>(_lyric_lines.size()));
            auto lines = music_lyric_layout::wrap(lyric.text, width,
                [](const std::string &text) { return system_font::measure(text); });
            for (auto &line : lines) _lyric_lines.push_back(std::move(line));
        }
        _lyric_starts.push_back(static_cast<int>(_lyric_lines.size()));
    }

    int lyric_window(int line_height)
    {
        const auto area = lyric_area();
        const int pitch = line_height + std::max(4, design_y(6));
        const int capacity = std::max(1, (area.bottom - area.top +
                                         pitch - line_height) / pitch);
        prepare_lyric_lines(std::max(1, area.right - area.left - area.inset * 2));
        if (_lyrics.empty()) return 0;
        const int active = std::max(0, current_lyric_index());
        const uint32_t age = _elapsed_ms >= _lyrics[active].time_ms
            ? _elapsed_ms - _lyrics[active].time_ms : 0;
        return music_lyric_layout::window_start(_lyric_starts[active],
            _lyric_starts[active + 1], static_cast<int>(_lyric_lines.size()), capacity, age);
    }

    void draw_wrapped_lyrics()
    {
        const auto area = lyric_area();
        const int width = area.right - area.left, height = area.bottom - area.top;
        if (width <= 0 || height <= 0) return;
        M5.Display.fillRect(area.left, area.top, width, height, TFT_WHITE);
        if (_lyrics.empty()) return;
        const int line_height = system_font::line_height();
        const int pitch = line_height + std::max(4, design_y(6));
        const int capacity = std::max(1, (height + pitch - line_height) / pitch);
        const int first = lyric_window(line_height);
        _last_lyric_window = first;
        const int count = std::min(capacity, static_cast<int>(_lyric_lines.size()) - first);
        const int text_height = count * pitch - (pitch - line_height);
        const int text_top = area.top + std::max(0, (height - text_height) / 2);
        const int active = current_lyric_index();

        int32_t clip_x, clip_y, clip_w, clip_h;
        M5.Display.getClipRect(&clip_x, &clip_y, &clip_w, &clip_h);
        const int x = std::max(area.left, int(clip_x));
        const int y = std::max(area.top, int(clip_y));
        M5.Display.setClipRect(x, y,
            std::max(0, std::min(area.right, int(clip_x + clip_w)) - x),
            std::max(0, std::min(area.bottom, int(clip_y + clip_h)) - y));
        for (int row = 0; row < count; ++row) {
            const int line = first + row;
            const bool selected = _view == ContentView::Lyrics && active >= 0 &&
                line >= _lyric_starts[active] && line < _lyric_starts[active + 1];
            const int top = text_top + row * pitch;
            if (selected) {
                M5.Display.fillRect(area.left + design_x(6), top - 2,
                                    width - design_x(12), pitch, TFT_BLACK);
            }
            system_font::draw_center(M5.Display, _lyric_lines[line],
                (area.left + area.right) / 2, top, selected ? TFT_WHITE : TFT_BLACK);
        }
        M5.Display.setClipRect(clip_x, clip_y, clip_w, clip_h);
    }

    void draw_current_lyric_section()
    {
        if (_view == ContentView::Cover) draw_wrapped_lyrics();
    }

    void draw_cover_view()
    {
        int x = design_x(ALBUM_ART_X);
        int y = design_y(ALBUM_ART_Y);
        int width = std::max(1, design_x(ALBUM_ART_SIZE));
        int height = std::max(1, design_y(ALBUM_ART_SIZE));
        M5.Display.fillRect(x, y, width, height, TFT_WHITE);

        bool drew_cover = false;
        if (!_cover_thumbnail_path.empty()) {
            drew_cover = p4_jpeg::draw_file(M5.Display, _cover_thumbnail_path.c_str(),
                                             x, y, width, height);
            if (!drew_cover) {
                float scale = std::min((float)width / 360.0f, (float)height / 360.0f);
                drew_cover = M5.Display.drawJpgFile(_cover_thumbnail_path.c_str(),
                                                     x, y, width, height,
                                                     0, 0, scale, scale);
            }
        }
        if (!drew_cover) {
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::NoCover), x + width / 2,
                                     y + std::max(0, (height - system_font::line_height()) / 2),
                                     TFT_BLACK);
        }
        draw_current_lyric_section();
        draw_view_indicator();
    }

    void draw_lyrics_section()
    {
        if (_view != ContentView::Lyrics) return;
        draw_wrapped_lyrics();
        draw_view_indicator();
    }

    void draw_progress_bar(uint32_t progress_permille)
    {
        int left = design_x(PROGRESS_X0);
        int right = design_x(PROGRESS_X1);
        int y = design_y(PROGRESS_Y);
        int width = std::max(1, right - left + 1);
        int thin_height = std::max(1, design_y(2));
        int thick_height = std::max(thin_height + 2, design_y(4));
        int clear_height = thick_height + 4;

        M5.Display.fillRect(left, y - clear_height / 2, width, clear_height, TFT_WHITE);
        M5.Display.fillRect(left, y - thin_height / 2, width, thin_height, TFT_BLACK);

        int played_width = static_cast<int>((static_cast<int64_t>(width) *
                                             std::min<uint32_t>(progress_permille, 1000)) /
                                            1000);
        if (played_width > 0) {
            M5.Display.fillRect(left, y - thick_height / 2,
                                std::max(played_width, thick_height / 2),
                                thick_height, TFT_BLACK);
        }
    }

    void draw_progress_section(uint32_t progress_permille)
    {
        int left = design_x(PROGRESS_X0);
        int right = design_x(PROGRESS_X1);
        int clear_top = design_y(PROGRESS_Y - 7);
        int clear_bottom = design_y(PROGRESS_TEXT_Y) + system_font::line_height() + design_y(3);
        M5.Display.fillRect(left, clear_top, std::max(1, right - left + 1),
                            std::max(1, clear_bottom - clear_top), TFT_WHITE);

        draw_progress_bar(progress_permille);
        system_font::draw_left(M5.Display, format_playback_time(_elapsed_ms),
                               left, design_y(PROGRESS_TEXT_Y), TFT_BLACK);
        system_font::draw_right(M5.Display,
                                format_playback_time(_duration_ms, _duration_ms > 0),
                                right, design_y(PROGRESS_TEXT_Y), TFT_BLACK);
    }

    void update_progress_if_due()
    {
        if (_refresh_frozen) return;
        if (_current < 0 || audio_radio_get_state() != AUDIO_PLAYBACK_PLAYING) return;

        int64_t now = esp_timer_get_time();
        if (_last_progress_update_us != 0 &&
            now - _last_progress_update_us < PROGRESS_UPDATE_US) return;
        if (M5.Display.displayBusy()) return;
        _last_progress_update_us = now;

        uint32_t previous = _progress_permille;
        uint32_t previous_elapsed_second = _elapsed_ms / 1000;
        uint32_t previous_duration = _duration_ms;
        uint32_t progress = read_progress();
        int lyric_index = current_lyric_index();
        bool lyric_changed = _view != ContentView::List && !_lyrics.empty() &&
                             (lyric_index != _last_lyric_index ||
                              lyric_window(system_font::line_height()) != _last_lyric_window);
        if (progress == previous && _elapsed_ms / 1000 == previous_elapsed_second &&
            _duration_ms == previous_duration && !lyric_changed) return;

        M5.Display.startWrite();
        // Keep progress and lyric partial updates on the same waveform.
        // Switching between fast/fastest here causes a visible dark flash.
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_progress_section(progress);
        if (lyric_changed) {
            if (_view == ContentView::Cover) draw_current_lyric_section();
            else if (_view == ContentView::Lyrics) draw_lyrics_section();
        }
        // Drawing already records a rotation-correct dirty rectangle inside
        // Panel_EPD. Queue that range directly instead of merging these
        // logical portrait coordinates into the panel's landscape range.
        M5.Display.display();
        M5.Display.endWrite();
        _last_lyric_index = lyric_index;
    }

    void draw_message(const char *message)
    {
        M5.Display.startWrite();
        // This is the first refresh after entering Music. Use a full quality
        // waveform to clear remnants from whichever activity was visible.
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);

        _status_bar.draw(ui_locale::text(ui_locale::TextId::Music));
        system_font::draw_center(M5.Display, message, M5.Display.width() / 2,
                                 M5.Display.height() / 2 - 20, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_icon(MusicIconId id, int target_x, int target_y, bool active = false)
    {
        const IconBitmap1bpp *icon = music_icons::get(id);
        if (icon == nullptr) return;

        uint16_t foreground = TFT_BLACK;
        uint16_t background = TFT_WHITE;
        if (active) {
            int center_x = design_x(target_x);
            int center_y = design_y(target_y);
            int width = design_x(62);
            int height = design_y(58);
            M5.Display.fillRoundRect(center_x - width / 2,
                                     center_y - height / 2,
                                     width,
                                     height,
                                     std::max(4, design_x(8)),
                                     TFT_BLACK);
            foreground = TFT_WHITE;
            background = TFT_BLACK;
        }

        draw_icon_1bpp(M5.Display,
                       *icon,
                       design_x(target_x),
                       design_y(target_y),
                       foreground,
                       background,
                       std::max(1, design_x(icon->width)),
                       std::max(1, design_y(icon->height)));
    }

    void draw_track_list()
    {
        int left = design_x(COVER_X + LIST_INSET);
        int width = design_x(COVER_W - LIST_INSET * 2);
        int top = list_top();
        int row_height = list_row_height();
        int rows = visible_rows();

        if (_tracks.empty()) {
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::NoSupportedAudio),
                                     design_x(COVER_X + COVER_W / 2),
                                     design_y(COVER_Y + COVER_H / 2 - 20), TFT_BLACK);
            draw_view_indicator();
            return;
        }

        for (int row = 0; row < rows; ++row) {
            int index = _scroll + row;
            if (index >= (int)_tracks.size()) break;
            int y = top + row * row_height;
            bool selected = index == _focus;
            constexpr uint16_t foreground = TFT_BLACK;
            M5.Display.fillRect(left, y + 2, width, row_height - 4, TFT_WHITE);
            list_focus::draw(M5.Display, left, y, row_height, selected);
            if (index == _current) {
                const int right = left + width - design_x(8);
                const int center_y = y + row_height / 2;
                M5.Display.fillTriangle(right - 9, center_y - 6,
                                        right - 9, center_y + 6, right, center_y, TFT_BLACK);
            }
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(_tracks[index].label,
                                       width - list_focus::text_inset() - design_x(28)),
                                   left + list_focus::text_inset(), y + design_y(10), foreground);
        }

        char position[32] = {};
        snprintf(position, sizeof(position), "%d / %u", _focus + 1, (unsigned)_tracks.size());
        system_font::draw_center(M5.Display, position,
                                 design_x(COVER_X + COVER_W / 2),
                                 design_y(COVER_Y + COVER_H - 56), TFT_BLACK);
        draw_view_indicator();
    }

    void draw_now_playing_panel()
    {
        if (_view == ContentView::List) {
            draw_track_list();
            return;
        }
        if (_view == ContentView::Cover) {
            draw_cover_view();
            return;
        }
        draw_lyrics_section();
        if (_lyrics.empty()) draw_view_indicator();
    }

    bool update_controls_if_state_changed()
    {
        if (audio_radio_get_state() == _controls_state) return false;
        // Level-triggered on purpose: a refresh already in flight only means the
        // next poll does the work, so nothing is lost by waiting for the panel.
        if (M5.Display.displayBusy()) return false;

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_controls();
        M5.Display.display();
        M5.Display.endWrite();
        return true;
    }

    // The two glyphs this slot alternates between are not the same size - Play is
    // 40x50, Pause 28x36 - and draw_icon_1bpp paints only the bitmap's own extent,
    // with no fixed cell behind it. Drawing the smaller one over the larger
    // therefore leaves a ring of the old glyph. draw_all() hides that behind its
    // fillScreen; a partial repaint has to clear the union of the two first.
    void draw_play_pause_icon()
    {
        const IconBitmap1bpp *play = music_icons::get(MusicIconId::Play);
        const IconBitmap1bpp *pause = music_icons::get(MusicIconId::Pause);
        if (play != nullptr && pause != nullptr) {
            const int width = design_x(std::max(play->width, pause->width));
            const int height = design_y(std::max(play->height, pause->height));
            M5.Display.fillRect(design_x(342) - width / 2, design_y(MAIN_CONTROL_Y) - height / 2,
                                width, height, TFT_WHITE);
        }
        draw_icon(audio_radio_get_state() == AUDIO_PLAYBACK_PLAYING ? MusicIconId::Pause
                                                                   : MusicIconId::Play,
                  342, MAIN_CONTROL_Y);
    }

    void draw_controls()
    {
        audio_playback_state_t state = audio_radio_get_state();
        _controls_state = state;
        draw_icon(MusicIconId::Previous, 207, MAIN_CONTROL_Y);
        draw_play_pause_icon();
        draw_icon(MusicIconId::Next, 479, MAIN_CONTROL_Y);

        MusicIconId mode_icon = MusicIconId::Sequence;
        if (_play_mode == PlayMode::Shuffle) mode_icon = MusicIconId::Shuffle;
        else if (_play_mode == PlayMode::RepeatOne) mode_icon = MusicIconId::RepeatOne;
        draw_icon(mode_icon, 91, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::Snowflake, 207, BOTTOM_CONTROL_Y, _refresh_frozen);
        draw_icon(MusicIconId::Playlist, 342, BOTTOM_CONTROL_Y,
                  _view == ContentView::List);
        draw_icon(MusicIconId::VolumeDown, 474, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::VolumeUp, 579, BOTTOM_CONTROL_Y);
    }

    void draw_audio_output_selector()
    {
        app_status_bar::draw_audio_output(audio_radio_get_output_mode_name());
    }

    void cycle_audio_output()
    {
        audio_output_mode_t current = audio_radio_get_output_mode();
        audio_output_mode_t next = current == AUDIO_OUTPUT_SPEAKER
                                       ? AUDIO_OUTPUT_USB
                                       : (current == AUDIO_OUTPUT_USB
                                              ? AUDIO_OUTPUT_AUTO
                                              : AUDIO_OUTPUT_SPEAKER);
        esp_err_t ret = audio_radio_set_output_mode(next);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Audio output switch failed: %s", esp_err_to_name(ret));
        }
        draw_all();
    }

    void draw_all()
    {
        ensure_focus_visible();
        // A full redraw also clears any transient volume toast.
        _volume_toast_pending = false;
        _volume_toast_drawn = false;
        _volume_toast_hide_us = 0;
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);

        _status_bar.draw(ui_locale::text(ui_locale::TextId::Music));

        std::string title;
        if (_view == ContentView::List) title = ui_locale::text(ui_locale::TextId::SongList);
        else if (_current >= 0) title = track_title(_tracks[_current].path);
        else title = ui_locale::text(ui_locale::TextId::SelectSong);
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(title, std::max(0,app_status_bar_layout::audio_output(M5.Display.width()).x-design_x(TITLE_X)-16)),
                               design_x(TITLE_X), std::max(app_status_bar::height+8,design_y(TITLE_Y)), TFT_BLACK);
        draw_audio_output_selector();

        M5.Display.drawRect(design_x(COVER_X), design_y(COVER_Y),
                            design_x(COVER_W), design_y(COVER_H), TFT_BLACK);
        draw_now_playing_panel();

        // A frozen screen must not advance its cached playback time merely
        // because another interactive control caused a full redraw.
        draw_progress_section(_refresh_frozen ? _progress_permille : read_progress());

        draw_controls();

        M5.Display.display();
        M5.Display.endWrite();
        _last_progress_update_us = esp_timer_get_time();
        _last_lyric_index = current_lyric_index();
    }

    void draw_refresh_freeze_control()
    {
        int center_x = design_x(207);
        int center_y = design_y(BOTTOM_CONTROL_Y);
        int width = design_x(64);
        int height = design_y(60);
        M5.Display.fillRect(center_x - width / 2, center_y - height / 2,
                            width, height, TFT_WHITE);
        draw_icon(MusicIconId::Snowflake, 207, BOTTOM_CONTROL_Y, _refresh_frozen);
    }

    void toggle_refresh_freeze()
    {
        _refresh_frozen = !_refresh_frozen;
        uint32_t progress = _progress_permille;
        if (!_refresh_frozen) progress = read_progress();

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        draw_refresh_freeze_control();
        if (!_refresh_frozen) {
            draw_progress_section(progress);
            if (_view == ContentView::Cover) draw_current_lyric_section();
            else if (_view == ContentView::Lyrics) draw_lyrics_section();
        }
        M5.Display.display();
        M5.Display.endWrite();
        _last_progress_update_us = esp_timer_get_time();
        _last_lyric_index = current_lyric_index();
    }
};

}  // namespace

void launch_music(const char *start_path)
{
    ActivityManager::instance().push(new MusicActivity(start_path));
}
