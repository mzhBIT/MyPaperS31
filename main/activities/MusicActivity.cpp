#include "activities/MusicActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "audio/audio_file_formats.h"
#include "audio/audio_radio.h"
#include "audio/embedded_lyrics.h"
#include "ui/SystemFont.h"
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
constexpr int LIST_FOOTER_H = 42;
constexpr int LYRICS_TOP = COVER_Y + 18;
constexpr int LYRICS_BOTTOM = COVER_Y + COVER_H - 18;
constexpr int LYRICS_ROWS = 9;
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

    void on_create() override
    {
        draw_message("正在扫描音频文件...");
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
        if (_tracks.empty()) _status = "SD卡中没有可播放的音频文件";
        draw_all();
    }

    void loop() override
    {
        audio_playback_state_t state = audio_radio_get_state();
        if (state == AUDIO_PLAYBACK_FINISHED && !_terminal_handled && _current >= 0) {
            _terminal_handled = true;
            play_next();
            return;
        } else if (state == AUDIO_PLAYBACK_ERROR && !_terminal_handled) {
            _terminal_handled = true;
            _status = "播放失败";
            draw_all();
            return;
        }
        if (update_volume_toast()) return;
        update_progress_if_due();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::SwipeUp) {
            if (_show_list) page_down();
            return _show_list;
        }
        if (event.type == TouchEventType::SwipeDown) {
            if (_show_list) page_up();
            return _show_list;
        }
        if (event.type != TouchEventType::Tap) return false;

        if (event.x < design_x(100) && event.y < design_y(130)) {
            ActivityManager::instance().pop();
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
            set_mode(true);
            return true;
        }
        if (hit_center(event.x, event.y, 207, BOTTOM_CONTROL_Y, 52, 46)) {
            set_mode(false);
            return true;
        }
        if (hit_center(event.x, event.y, 342, BOTTOM_CONTROL_Y, 52, 46)) {
            _show_list = !_show_list;
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
            set_mode(!_random);
            return true;
        }
        if (_tracks.empty()) return false;
        if (key == APP_KEY_UP && _focus > 0) {
            _show_list = true;
            --_focus;
            ensure_focus_visible();
            draw_all();
            return true;
        }
        if (key == APP_KEY_DOWN && _focus + 1 < (int)_tracks.size()) {
            _show_list = true;
            ++_focus;
            ensure_focus_visible();
            draw_all();
            return true;
        }
        if (key == APP_KEY_OK) {
            if (_show_list && _current != _focus) play_index(_focus);
            else toggle_pause();
            return true;
        }
        return false;
    }

private:
    std::string _initial_path;
    std::vector<Track> _tracks;
    std::vector<int> _shuffle_order;
    std::vector<embedded_lyrics::Line> _lyrics;
    int _shuffle_pos = -1;
    int _focus = 0;
    int _scroll = 0;
    int _current = -1;
    bool _random = false;
    bool _show_list = true;
    bool _terminal_handled = true;
    uint32_t _progress_permille = 0;
    uint32_t _elapsed_ms = 0;
    uint32_t _duration_ms = 0;
    int64_t _last_progress_update_us = 0;
    int _last_lyric_index = -2;
    int _volume_toast_value = 0;
    int64_t _volume_toast_hide_us = 0;
    bool _volume_toast_pending = false;
    bool _volume_toast_drawn = false;
    std::string _status;

    bool hit_center(int x, int y, int center_x, int center_y, int half_w, int half_h) const
    {
        return x >= design_x(center_x - half_w) && x <= design_x(center_x + half_w) &&
               y >= design_y(center_y - half_h) && y <= design_y(center_y + half_h);
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
        if (!_show_list || x < design_x(COVER_X) || x > design_x(COVER_X + COVER_W)) return -1;
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

    void set_mode(bool random)
    {
        _random = random;
        if (_random && !_tracks.empty()) {
            rebuild_shuffle(_current >= 0 ? _current : _focus);
        } else {
            _shuffle_order.clear();
            _shuffle_pos = -1;
        }
        // The selected mode is already visible on the active control icon.
        _status.clear();
        draw_all();
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
        _status = "正在打开...";
        esp_err_t ret = audio_radio_play_file(_tracks[index].path.c_str());
        if (ret == ESP_OK) {
            embedded_lyrics::load(_tracks[index].path.c_str(), &_lyrics);
            _current = index;
            _show_list = false;
            _terminal_handled = false;
            _status.clear();
            if (_random && !keep_shuffle_order) rebuild_shuffle(index);
            ESP_LOGI(TAG, "Playing %d/%u: %s", index + 1,
                     (unsigned)_tracks.size(), _tracks[index].path.c_str());
        } else {
            _lyrics.clear();
            _terminal_handled = true;
            char message[64] = {};
            snprintf(message, sizeof(message), "播放失败: %s", esp_err_to_name(ret));
            _status = message;
            ESP_LOGE(TAG, "Failed to play %s: %s", _tracks[index].path.c_str(),
                     esp_err_to_name(ret));
        }
        draw_all();
    }

    void play_next()
    {
        if (_tracks.empty()) return;
        int next = 0;
        if (_random) {
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

    void play_previous()
    {
        if (_tracks.empty()) return;
        int previous = 0;
        if (_random) {
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
            snprintf(message, sizeof(message), "控制失败: %s", esp_err_to_name(ret));
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
            _volume_toast_value = volume;
            _volume_toast_pending = true;
            update_volume_toast();
        } else {
            char message[64] = {};
            snprintf(message, sizeof(message), "音量调节失败: %s", esp_err_to_name(ret));
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
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
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
            case AUDIO_PLAYBACK_PLAYING: return "播放中";
            case AUDIO_PLAYBACK_PAUSED: return "已暂停";
            case AUDIO_PLAYBACK_FINISHED: return "播放完成";
            case AUDIO_PLAYBACK_ERROR: return "播放失败";
            case AUDIO_PLAYBACK_STOPPED: return "已停止";
            default: return _tracks.empty() ? "没有可播放的歌曲" : "请选择歌曲";
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

    void draw_lyrics_section()
    {
        if (_lyrics.empty() || _show_list) return;

        int left = design_x(COVER_X + 10);
        int right = design_x(COVER_X + COVER_W - 10);
        int top = design_y(LYRICS_TOP);
        int bottom = design_y(LYRICS_BOTTOM);
        int width = std::max(1, right - left);
        int row_height = std::max(system_font::line_height() + 6,
                                  (bottom - top) / LYRICS_ROWS);
        M5.Display.fillRect(left, top, width, std::max(1, bottom - top), TFT_WHITE);

        int active = current_lyric_index();
        int first = (active >= 0 ? active : 0) - LYRICS_ROWS / 2;
        for (int row = 0; row < LYRICS_ROWS; ++row) {
            int index = first + row;
            if (index < 0 || index >= static_cast<int>(_lyrics.size())) continue;
            int y = top + row * row_height;
            bool selected = index == active;
            uint16_t foreground = selected ? TFT_WHITE : TFT_BLACK;
            if (selected) {
                M5.Display.fillRoundRect(left + design_x(6), y + 2,
                                         width - design_x(12), row_height - 4,
                                         std::max(3, design_x(6)), TFT_BLACK);
            }
            std::string text = system_font::ellipsize(_lyrics[index].text,
                                                       width - design_x(36));
            system_font::draw_center(
                M5.Display, text, (left + right) / 2,
                y + std::max(0, (row_height - system_font::line_height()) / 2),
                foreground);
        }
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
        bool lyric_changed = !_show_list && !_lyrics.empty() &&
                             lyric_index != _last_lyric_index;
        if (progress == previous && _elapsed_ms / 1000 == previous_elapsed_second &&
            _duration_ms == previous_duration && !lyric_changed) return;

        M5.Display.startWrite();
        // Keep progress and lyric partial updates on the same waveform.
        // Switching between fast/fastest here causes a visible dark flash.
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        draw_progress_section(progress);
        if (lyric_changed) draw_lyrics_section();
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
        system_font::draw_left(M5.Display, "Music", design_x(TITLE_X), design_y(TITLE_Y), TFT_BLACK);
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
            system_font::draw_center(M5.Display, "未找到支持的音频文件",
                                     design_x(COVER_X + COVER_W / 2),
                                     design_y(COVER_Y + COVER_H / 2 - 20), TFT_BLACK);
            return;
        }

        for (int row = 0; row < rows; ++row) {
            int index = _scroll + row;
            if (index >= (int)_tracks.size()) break;
            int y = top + row * row_height;
            bool selected = index == _focus;
            uint16_t background = selected ? TFT_BLACK : TFT_WHITE;
            uint16_t foreground = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRect(left, y + 2, width, row_height - 4, background);
            if (index == _current) {
                M5.Display.fillRect(left + design_x(5), y + design_y(10),
                                    std::max(2, design_x(4)), row_height - design_y(20), foreground);
            }
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(_tracks[index].label, width - design_x(28)),
                                   left + design_x(18), y + design_y(10), foreground);
        }

        char position[32] = {};
        snprintf(position, sizeof(position), "%d / %u", _focus + 1, (unsigned)_tracks.size());
        system_font::draw_center(M5.Display, position,
                                 design_x(COVER_X + COVER_W / 2),
                                 design_y(COVER_Y + COVER_H - 38), TFT_BLACK);
    }

    void draw_now_playing_panel()
    {
        int center_x = design_x(COVER_X + COVER_W / 2);
        if (_show_list) {
            draw_track_list();
            return;
        }

        if (!_lyrics.empty()) {
            draw_lyrics_section();
            return;
        }

        const char *placeholder = _tracks.empty() ? "没有可播放的歌曲" : "暂无封面或歌词";
        system_font::draw_center(M5.Display, placeholder, center_x,
                                 design_y(COVER_Y + COVER_H / 2 - 42), TFT_BLACK);
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(state_text(), design_x(COVER_W - 60)),
                                 center_x, design_y(COVER_Y + COVER_H / 2 + 20), TFT_BLACK);
        if (_current >= 0) {
            char position[32] = {};
            snprintf(position, sizeof(position), "%d / %u", _current + 1, (unsigned)_tracks.size());
            system_font::draw_center(M5.Display, position, center_x,
                                     design_y(COVER_Y + COVER_H / 2 + 76), TFT_BLACK);
        }
    }

    void draw_controls()
    {
        audio_playback_state_t state = audio_radio_get_state();
        draw_icon(MusicIconId::Previous, 207, MAIN_CONTROL_Y);
        draw_icon(state == AUDIO_PLAYBACK_PLAYING ? MusicIconId::Pause : MusicIconId::Play,
                  342, MAIN_CONTROL_Y);
        draw_icon(MusicIconId::Next, 479, MAIN_CONTROL_Y);

        draw_icon(MusicIconId::Shuffle, 91, BOTTOM_CONTROL_Y, _random);
        draw_icon(MusicIconId::Sequence, 207, BOTTOM_CONTROL_Y, !_random);
        draw_icon(MusicIconId::Playlist, 342, BOTTOM_CONTROL_Y, _show_list);
        draw_icon(MusicIconId::VolumeDown, 474, BOTTOM_CONTROL_Y);
        draw_icon(MusicIconId::VolumeUp, 579, BOTTOM_CONTROL_Y);
    }

    void draw_all()
    {
        ensure_focus_visible();
        // A full redraw also clears any transient volume toast.
        _volume_toast_pending = false;
        _volume_toast_drawn = false;
        _volume_toast_hide_us = 0;
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);

        std::string title;
        if (_show_list) title = "歌曲列表";
        else if (_current >= 0) title = track_title(_tracks[_current].path);
        else title = "选择歌曲";
        system_font::draw_left(M5.Display,
                               system_font::ellipsize(title, design_x(530)),
                               design_x(TITLE_X), design_y(TITLE_Y), TFT_BLACK);

        M5.Display.drawRect(design_x(COVER_X), design_y(COVER_Y),
                            design_x(COVER_W), design_y(COVER_H), TFT_BLACK);
        draw_now_playing_panel();

        draw_progress_section(read_progress());

        draw_controls();

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
