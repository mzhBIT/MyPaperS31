#include "activities/XhsReaderActivity.h"

#include "activities/ActivityManager.h"
#include "activities/HomeActivity.h"
#include "activities/StatusBarActivity.h"
#include "activities/TextInputActivity.h"
#include "time/time_basis.h"
#include "ui/Utf8Text.h"
#include "ui/ListFocus.h"
#include "ui/weread_icons.h"
#include "xhs/XhsReader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <esp_log.h>
#include <utility>

namespace {

using ui_locale::TextId;
using ui_locale::text;
using xhs::Action;

constexpr char tag[] = "XHS_READER";
constexpr int pad = 24;
constexpr int gap = 18;
constexpr int header = app_status_bar::height;
constexpr int byline_height = 66;
constexpr int gallery_footer = 44;
constexpr uint32_t double_tap_ms = 360;
constexpr int double_tap_slop = 48;
constexpr uint16_t soft_gray = 0xef7d;
constexpr uint16_t separator = 0xce59;
constexpr size_t max_cards = 300;
// Includes the displayed photo. Download/decoder work buffers are temporary.
constexpr size_t album_cache_budget = 4 * 1024 * 1024;

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x+w && py >= y && py < y+h; }
};

void centered(const std::string &value, int x, int y, int px, uint16_t color = TFT_BLACK)
{
    system_font::draw_left_sized(M5.Display, value, x - system_font::measure(value, px)/2,
                                system_font::centered_y(value, y, px), px, color);
}

void chevron(int cx, int cy, bool left)
{
    const int tip = cx + (left ? -8 : 8), end = cx + (left ? 8 : -8);
    for (int offset = -1; offset <= 1; ++offset) {
        M5.Display.drawLine(tip+offset, cy, end+offset, cy-15, TFT_BLACK);
        M5.Display.drawLine(tip+offset, cy, end+offset, cy+15, TFT_BLACK);
    }
}

void likes_at(const std::string &likes, int x, int center_y, int px)
{
    if (likes.empty()) return;
    M5.Display.drawBitmap(x, center_y - weread_icons::heart_size/2, weread_icons::heart,
                         weread_icons::heart_size, weread_icons::heart_size, TFT_BLACK);
    system_font::draw_left_sized(M5.Display, likes, x + weread_icons::heart_size + 4,
                                system_font::centered_y(likes, center_y, px), px, TFT_BLACK);
}

int likes_width(const std::string &likes, int px)
{
    return likes.empty() ? 0 : weread_icons::heart_size + 4 + system_font::measure(likes, px);
}

void draw_image(const Rect &box, const xhs::Image &image, bool fullscreen = false)
{
    M5.Display.fillRect(box.x, box.y, box.w, box.h, fullscreen ? TFT_WHITE : soft_gray);
    if (!image.pixels) return;
    static const auto palette = [] {
        std::array<lgfx::rgb565_t, 16> colors{};
        for (int i = 0; i < 16; ++i) colors[i] = M5.Display.color565(i*17, i*17, i*17);
        return colors;
    }();
    if (fullscreen || image.width > box.w || image.height > box.h) {
        const float scale = std::min(float(box.w)/image.width, float(box.h)/image.height);
        if (scale != 1.0f) {
            M5.Display.pushImageRotateZoom(box.x + box.w*0.5f, box.y + box.h*0.5f,
                image.width*0.5f, image.height*0.5f, 0, scale, scale, image.width, image.height,
                image.pixels.get(), lgfx::color_depth_t::palette_4bit, palette.data());
            return;
        }
    }
    const int x = box.x + (box.w - image.width)/2;
    const int y = box.y + (box.h - image.height)/2;
    if ((image.width & 1) == 0) {
        M5.Display.pushImage(x, y, image.width, image.height, image.pixels.get(),
                            lgfx::color_depth_t::palette_4bit, palette.data());
    } else {
        const int stride = (image.width + 1) / 2;
        for (int row = 0; row < image.height; ++row)
            M5.Display.pushImage(x, y + row, image.width, 1, image.pixels.get() + row*stride,
                                lgfx::color_depth_t::palette_4bit, palette.data());
    }
}

TextId error_text(const std::string &error)
{
    if (error == "invalid_token") return TextId::XhsInvalidToken;
    if (error == "login_required") return TextId::XhsLoginRequired;
    if (error == "session_expired" || error == "job_missing") return TextId::XhsSessionExpired;
    if (error == "network_error" || error == "timeout") return TextId::XhsConnectionFailed;
    if (error == "queue_full" || error == "detail_rate_limited") return TextId::XhsBusy;
    if (error == "detail_unavailable" || error == "unsupported_note_type" || error == "note_not_found")
        return TextId::XhsNoteUnavailable;
    return TextId::XhsRequestFailed;
}

struct Line {
    std::string text;
    size_t next;
};

Line line_at(const std::string &value, size_t from, int width, int pixels)
{
    size_t end = from, space = std::string::npos;
    int used = 0;
    while (end < value.size()) {
        if (value[end] == '\n') return {value.substr(from, end-from), end+1};
        const size_t next = ui::utf8::next_boundary(value, end);
        used += system_font::measure(value.substr(end, next-end), pixels);
        if (end != from && used > width) {
            if (space != std::string::npos) return {value.substr(from, space-from), space+1};
            break;
        }
        // Wrap the current Latin word at a preceding space, but avoid using
        // stale spaces from much earlier in a CJK line.
        const unsigned char c = value[end];
        if (c == ' ') space = end;
        else if (c >= 0x80) space = std::string::npos;
        end = next;
    }
    return {value.substr(from, end-from), end};
}

std::string publication(const xhs::Note &note)
{
    std::string value = note.published_at;
    int year, month, day, hour, minute, second;
    if (value.size() >= 19 && std::sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d",
            &year, &month, &day, &hour, &minute, &second) == 6 &&
        year >= 1970 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
        hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 60) {
        std::tm date{};
        date.tm_year = year - 1900;
        date.tm_mon = month - 1;
        date.tm_mday = day;
        date.tm_hour = hour;
        date.tm_min = minute;
        date.tm_sec = second;
        char label[32];
        if (time_basis_format_utc(timegm(&date), label, sizeof(label), nullptr)) value = label;
    }
    if (!note.ip_location.empty()) {
        if (!value.empty()) value += " · ";
        value += note.ip_location;
    }
    return value;
}

class Reader final : public StatusBarActivity {
public:
    ~Reader() override {
        cancel_tasks();
        request_home_quality_refresh();
    }

    void on_create() override {
        if (_config.ready()) start_data(Action::Open);
        else draw();
    }

    void on_show() override {
        if (_shown) refresh_config();
        _shown = true;
    }

    bool can_standby() const override {
        return (!_data || _data->done.load()) && (!_media || _media->done.load());
    }

    void loop() override {
        if (!M5.Display.displayBusy()) {
            if (_data && _data->done.load(std::memory_order_acquire)) finish_data();
            else if (_media) consume_images();
            if (!_media) start_images();
        }
        if (!_fullscreen) StatusBarActivity::loop();
    }

    bool on_touch(const TouchEvent &event) override {
        if (event.type != TouchEventType::Tap) _tap_pending = false;
        if (_fullscreen) {
            if (event.type == TouchEventType::SwipeLeft || event.type == TouchEventType::SwipeRight ||
                (event.type == TouchEventType::Back && event.dx != 0)) {
                step_image(event.dx < 0 || event.type == TouchEventType::SwipeLeft ? 1 : -1);
            } else if (event.type == TouchEventType::Tap) {
                image_tap(event);
            } else if (event.type == TouchEventType::Back) {
                back();
            }
            return true;
        }
        if (event.type == TouchEventType::Tap && _detail && _loaded && _text_page == 0 &&
            !_note.images.empty() && gallery().contains(event.x, event.y)) {
            image_tap(event);
            return true;
        }
        _tap_pending = false;
        if (event.type == TouchEventType::Back) { back(); return true; }
        if (status_bar_touch(event)) return true;
        if (event.type == TouchEventType::SwipeDown || event.type == TouchEventType::SwipeUp) {
            move(event.type == TouchEventType::SwipeUp ? 1 : -1);
            return true;
        }
        if (_detail && _loaded && !_note.images.empty() && _text_page == 0) {
            if ((event.type == TouchEventType::SwipeLeft || event.type == TouchEventType::SwipeRight) &&
                gallery().contains(event.start_x, event.start_y)) {
                step_image(event.type == TouchEventType::SwipeLeft ? 1 : -1);
                return true;
            }
            if (event.type == TouchEventType::Tap && footer().contains(event.x, event.y)) {
                if (event.x < pad + 70) step_image(-1);
                else if (event.x >= M5.Display.width() - pad - 70) step_image(1);
                return true;
            }
        }
        if (event.type != TouchEventType::Tap) return false;
        if (_detail && event.x < 84 && event.y >= header && event.y < header + byline_height) {
            back();
            return true;
        }
        if (_notice_hit.contains(event.x, event.y) && !_data) {
            if (!_config.ready()) launch_xhs_settings();
            else if (recoverable_session()) reopen_list();
            else if (_retry == Action::Detail) start_data(Action::Detail);
            else start_data(_retry);
            return true;
        }
        if (!_detail) {
            for (size_t i = 0; i < xhs::batch_size && _page + i < _cards.size(); ++i) {
                if (card_box(i).contains(event.x, event.y)) {
                    open_note(_cards[_page + i]);
                    return true;
                }
            }
        }
        return false;
    }

    bool on_key(int key) override {
        if (key == APP_KEY_BACK) { back(); return true; }
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) return on_navigation_key(key == APP_KEY_DOWN ? 1 : -1);
        return false;
    }

    bool on_navigation_key(int direction) override {
        _tap_pending = false;
        if (_fullscreen) step_image(direction > 0 ? 1 : -1);
        else move(direction > 0 ? 1 : -1);
        return true;
    }

private:
    xhs::Config _config = xhs::load_config();
    std::shared_ptr<xhs::Network> _network = xhs::make_network();
    std::shared_ptr<xhs::Task> _data, _media;
    std::vector<xhs::Card> _cards;
    std::string _session, _note_id, _placeholder_author, _placeholder_likes, _error;
    xhs::Note _note;
    std::array<xhs::Image, xhs::batch_size> _covers;
    std::array<bool, xhs::batch_size> _cover_ready{};
    std::array<size_t, xhs::batch_size> _slots{};
    xhs::Image _album;
    struct CachedPhoto {
        size_t index;
        xhs::Image image;
        bool full_size;
    };
    // Oldest first. The displayed image lives separately until we leave it.
    std::vector<CachedPhoto> _album_cache;
    bool _fullscreen = false, _album_full_size = false, _media_full_size = false;
    bool _tap_pending = false;
    uint32_t _tap_time = 0;
    int _tap_x = 0, _tap_y = 0;
    size_t _page = 0, _image_index = 0, _consumed = 0, _text_page = 0;
    struct ArticlePosition { size_t title = 0, body = 0; };
    std::vector<ArticlePosition> _article_pages{ArticlePosition{}};
    bool _next_text = false, _detail = false, _loaded = false, _album_ready = false, _shown = false;
    bool _images_needed = false, _screen_has_images = false;
    Action _retry = Action::Open;
    Rect _notice_hit{};

    static void begin(epd_mode_t mode) {
        M5.Display.startWrite();
        M5.Display.setEpdMode(mode);
    }
    static void end() { M5.Display.endWrite(); }

    void cancel_tasks() {
        if (_data) _data->cancel.store(true);
        if (_media) _media->cancel.store(true);
    }

    void refresh_config() {
        const bool was_ready = _config.ready();
        _config = xhs::load_config();
        if (!was_ready && _config.ready() && !_data) start_data(Action::Open);
        else draw();
    }

    Rect card_box(size_t slot) const {
        const int width = (M5.Display.width() - 2*pad - gap)/2;
        const int height = (M5.Display.height() - header - 2*pad - 2*gap)/3;
        return {pad + int(slot%2)*(width+gap), header+pad+int(slot/2)*(height+gap), width, height};
    }

    Rect cover_box(size_t slot) const {
        auto box = card_box(slot);
        const int title_px = std::clamp(system_font::pixel_size(), 24, 35);
        box.h -= 2*system_font::line_height(title_px) + 38;
        return box;
    }

    Rect gallery() const {
        const int width = M5.Display.width() - 2*pad;
        return {pad, header + byline_height + 16, width, width*2/3};
    }

    Rect album_box() const {
        if (_fullscreen) return {0, 0, M5.Display.width(), M5.Display.height()};
        return gallery();
    }

    Rect footer() const {
        auto box = gallery();
        return {box.x, box.y+box.h, box.w, gallery_footer};
    }

    void draw_card(size_t i) {
        const auto &card = _cards[_page + i];
        const auto box = card_box(i), cover = cover_box(i);
        const int title_px = std::clamp(system_font::pixel_size(), 24, 35);
        const int meta_px = std::clamp(title_px-4, 21, 28);
        draw_image(cover, _covers[i]);
        size_t offset = 0;
        const int line_height = system_font::line_height(title_px);
        for (int row = 0; row < 2 && offset < card.title.size(); ++row) {
            auto line = line_at(card.title, offset, box.w, title_px);
            if (row == 1 && line.next < card.title.size())
                line.text = app_status_bar::fit(line.text + "...", box.w, title_px);
            system_font::draw_left_sized(M5.Display, line.text, box.x, cover.y+cover.h+4+row*line_height, title_px);
            offset = line.next;
        }
        const int center_y = box.y + box.h - 17;
        const auto likes = app_status_bar::fit(card.likes, box.w/3, meta_px);
        const int like_w = likes_width(likes, meta_px);
        auto author = app_status_bar::fit(card.author, box.w - like_w - 8, meta_px);
        system_font::draw_left_sized(M5.Display, author, box.x,
                                    system_font::centered_y(author, center_y, meta_px), meta_px);
        likes_at(likes, box.x + box.w - like_w, center_y, meta_px);
        M5.Display.drawFastHLine(box.x, box.y+box.h-1, box.w, separator);
    }

    void draw_byline() {
        const int center_y = header + byline_height/2;
        const int px = std::clamp(system_font::pixel_size(), 24, 36);
        chevron(pad+16, center_y, true);
        const auto &author = _loaded ? _note.author : _placeholder_author;
        const auto &likes = _loaded && !_note.likes.empty() ? _note.likes : _placeholder_likes;
        const auto likes_label = app_status_bar::fit(likes, 142, px-2);
        const auto name = app_status_bar::fit(author, M5.Display.width()-2*pad-62-likes_width(likes_label,px-2), px);
        const int x = pad + 54;
        system_font::draw_left_sized(M5.Display, name, x, system_font::centered_y(name, center_y, px), px);
        likes_at(likes_label, x+system_font::measure(name, px)+18, center_y, px-2);
        M5.Display.drawFastHLine(pad, header+byline_height-1, M5.Display.width()-2*pad, separator);
    }

    void draw_gallery() {
        const auto box = gallery(), bottom = footer();
        draw_image(box, _album);
        if (_album_ready && !_album.pixels)
            centered(text(TextId::XhsImageFailed), box.x+box.w/2, box.y+box.h/2, 24);
        M5.Display.fillRect(bottom.x, bottom.y, bottom.w, bottom.h, TFT_WHITE);
        if (_image_index > 0) chevron(bottom.x+24, bottom.y+bottom.h/2, true);
        if (_image_index+1 < _note.images.size())
            chevron(bottom.x+bottom.w-24, bottom.y+bottom.h/2, false);
        centered(std::to_string(_image_index+1) + " / " + std::to_string(_note.images.size()),
                 bottom.x+bottom.w/2, bottom.y+bottom.h/2, 23);
        M5.Display.drawFastHLine(bottom.x, bottom.y+bottom.h-1, bottom.w, separator);
    }

    void draw_album() {
        if (_text_page != 0 || _note.images.empty()) return;
        _screen_has_images = bool(_album.pixels);
        const auto box = album_box();
        begin(epd_mode_t::epd_hd);
        if (_fullscreen) {
            draw_image(box, _album, true);
            if (_album_ready && !_album.pixels)
                centered(text(TextId::XhsImageFailed), box.w/2, box.h/2, 24);
        } else draw_gallery();
        M5.Display.display(box.x, box.y, box.w, box.h + (_fullscreen ? 0 : gallery_footer));
        end();
    }

    void draw_article() {
        const int px = system_font::pixel_size();
        const int line_h = system_font::line_height(px) + 5;
        const int width = M5.Display.width() - 2*pad;
        const int last_y = M5.Display.height() - pad;
        int y = header + byline_height + 16;
        if (_text_page == 0 && !_note.images.empty()) {
            draw_gallery();
            const auto bottom = footer();
            y = bottom.y + bottom.h + 18;
        }

        auto position = _article_pages[_text_page];
        const int title_px = std::min(px+8, 51);
        const int title_height = system_font::line_height(title_px);
        const auto pub = publication(_note);
        const int pub_px = std::clamp(px-5, 21, 28);
        const int pub_height = pub.empty() ? 0 : system_font::line_height(pub_px) + 6;
        const bool has_title = position.title < _note.title.size();
        while (position.title < _note.title.size() && y+title_height <= last_y) {
            const auto line = line_at(_note.title, position.title, width, title_px);
            if (line.next == _note.title.size() && _note.body.empty() &&
                y+title_height+6+pub_height > last_y) break;
            system_font::draw_left_sized(M5.Display, line.text, pad, y, title_px);
            position.title = line.next;
            y += title_height;
        }
        if (has_title && position.title == _note.title.size()) y += 6;

        while (position.title == _note.title.size() && position.body < _note.body.size() &&
               y+line_h <= last_y) {
            const auto line = line_at(_note.body, position.body, width, px);
            if (line.next == _note.body.size() && y+line_h+pub_height > last_y) break;
            system_font::draw_left_sized(M5.Display, line.text, pad, y, px);
            position.body = line.next;
            y += line_h;
        }
        if (position.title == _note.title.size() && position.body == _note.body.size()) {
            const auto label = app_status_bar::fit(pub, width, pub_px);
            system_font::draw_left_sized(M5.Display, label, pad, y+6, pub_px, TFT_DARKGRAY);
            _next_text = false;
        } else {
            _next_text = true;
            _article_pages.resize(_text_page+1);
            _article_pages.push_back(position);
        }
    }

    void notice(const std::string &label, bool tappable, bool bottom) {
        const int px = std::clamp(system_font::pixel_size(), 24, 34);
        const int width = M5.Display.width();
        const int center_y = bottom ? M5.Display.height()-34 : M5.Display.height()/2;
        const int left = pad, height = 62;
        const auto fitted = app_status_bar::fit(label, width-2*pad-16, px);
        if (bottom) M5.Display.fillRect(left, center_y-height/2, width-2*pad, height, TFT_WHITE);
        centered(fitted, width/2, center_y, px);
        if (tappable) _notice_hit = {left,center_y-height/2,width-2*pad,height};
    }

    void draw() {
        _notice_hit = {};
        if (_fullscreen) {
            draw_album();
            return;
        }
        const bool has_images = _detail ? (_text_page == 0 && bool(_album.pixels)) :
            std::any_of(_covers.begin(), _covers.end(), [](const auto &image) { return bool(image.pixels); });
        begin(has_images || _screen_has_images ? epd_mode_t::epd_hd : epd_mode_t::epd_text);
        _screen_has_images = has_images;
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(text(TextId::Xiaohongshu));
        if (_detail) {
            draw_byline();
            if (_loaded) draw_article();
        } else {
            for (size_t i = 0; i < xhs::batch_size && _page+i < _cards.size(); ++i) draw_card(i);
        }

        const bool on_cards = !_detail && !_cards.empty();
        if (!_config.ready()) notice(text(TextId::XhsSetupRequired), true, false);
        else if (!_error.empty()) notice(text(error_text(_error)), true, on_cards);
        else if (_data) {
            const auto action = _data->action;
            notice(text(action == Action::Detail ? TextId::XhsLoadingNote :
                        action == Action::More ? TextId::XhsLoadingMore : TextId::XhsLoadingFeed),
                false, on_cards);
        } else if (!_detail && _cards.empty())
            notice(text(TextId::XhsNoItems), false, false);

        M5.Display.display();
        end();
    }

    void start_data(Action action) {
        if (_data) return;
        _error.clear();
        _retry = action;
        auto job = std::make_shared<xhs::Task>(action);
        if (action == Action::More || action == Action::Detail) job->session = _session;
        if (action == Action::Detail) job->note_id = _note_id;
        if (xhs::start(job, _config, _network)) _data = std::move(job);
        else _error = "request_failed";
        draw();
    }

    bool recoverable_session() const {
        return _error == "session_expired" || _error == "job_missing" ||
               _error == "login_required";
    }

    void reopen_list() {
        if (_detail) leave_detail();
        start_data(Action::Open);
    }

    void finish_data() {
        auto job = std::exchange(_data, {});
        if (!job->error.empty()) {
            _error = job->error;
        } else if (job->action == Action::Detail) {
            _note = std::move(job->note);
            _loaded = true;
            restart_images();
        } else {
            const bool append = job->action == Action::More;
            const size_t old_size = append ? _cards.size() : 0;
            if (!append) { _cards.clear(); _page = 0; }
            _session = std::move(job->session);
            for (auto &card : job->cards) {
                if (std::none_of(_cards.begin(), _cards.end(), [&](const auto &c) {return c.id == card.id;}))
                    _cards.push_back(std::move(card));
            }
            if (append && old_size == _cards.size()) {
                // A completed More may trigger one refresh. Refresh results
                // follow the replacement path, including when they are empty.
                start_data(Action::Refresh);
                return;
            }
            if (append) _page = (old_size/xhs::batch_size)*xhs::batch_size;
            clear_covers();
            // Match the service's maximum retained note IDs.
            if (_cards.size() > max_cards) {
                _cards.erase(_cards.begin(), _cards.begin()+xhs::batch_size);
                _page -= xhs::batch_size;
            }
            restart_images();
        }
        draw();
    }

    void clear_covers() {
        for (auto &image : _covers) image = {};
        _cover_ready.fill(false);
    }

    void restart_images() {
        if (_media) _media->cancel.store(true);
        _images_needed = true;
    }

    void start_images() {
        if (!_images_needed || !_config.ready() || _media) return;
        _images_needed = false;
        auto job = std::make_shared<xhs::Task>(Action::Images);
        auto add = [&](const xhs::Media &media, const Rect &box, size_t slot) {
            if (media.gray4.empty() && media.cdn.empty() && media.jpeg.empty()) return;
            _slots[job->image_count] = slot;
            job->requests[job->image_count++] = {media, box.w, box.h};
        };

        if (_detail) {
            if (_loaded && !_note.images.empty() && !_album_ready && _text_page == 0)
                add(_note.images[_image_index], album_box(), 0);
        } else {
            const size_t visible = std::min(xhs::batch_size, _cards.size()-_page);
            size_t cached = 0;
            for (size_t i = 0; i < visible; ++i) {
                if (_cover_ready[i]) ++cached;
                else add(_cards[_page+i].cover, cover_box(i), i);
            }
            if (visible) ESP_LOGI(tag, "feed covers page=%u queued=%u cached=%u unavailable=%u",
                unsigned(_page/xhs::batch_size+1), unsigned(job->image_count), unsigned(cached),
                unsigned(visible-cached-job->image_count));
        }
        if (!job->image_count) return;
        _consumed = 0;
        if (xhs::start(job, _config, _network)) {
            _media = std::move(job);
            _media_full_size = _fullscreen;
        } else {
            ESP_LOGW(tag, "%s image task could not start", _detail ? "detail" : "feed");
            if (_detail) { _album_ready = true; draw_album(); }
        }
    }

    void consume_images() {
        const bool canceled = _media->cancel.load();
        const bool done = _media->done.load(std::memory_order_acquire);
        const size_t ready = _media->images_ready.load(std::memory_order_acquire);
        if (!canceled && _detail && ((_consumed < ready) || (done && !_album_ready))) {
            if (_media->images[0].pixels) {
                _album = std::move(_media->images[0]);
                _album_full_size = _media_full_size;
                trim_album_cache();
            }
            _album_ready = true;
            draw_album();
        } else if (!canceled && !_detail && _consumed < ready) {
            begin(epd_mode_t::epd_hd);
            Rect refresh{M5.Display.width(), M5.Display.height(), 0, 0};
            for (size_t i = _consumed; i < ready; ++i) {
                const size_t slot = _slots[i];
                _covers[slot] = std::move(_media->images[i]);
                _cover_ready[slot] = bool(_covers[slot].pixels);
                if (!_cover_ready[slot]) {
                    ESP_LOGW(tag, "feed cover unavailable after download slot=%u", unsigned(slot+1));
                    continue;
                }
                const auto box = cover_box(slot);
                if (!refresh.w) refresh = box;
                else {
                    const int right = std::max(refresh.x+refresh.w, box.x+box.w);
                    const int bottom = std::max(refresh.y+refresh.h, box.y+box.h);
                    refresh.x = std::min(refresh.x, box.x);
                    refresh.y = std::min(refresh.y, box.y);
                    refresh.w = right-refresh.x;
                    refresh.h = bottom-refresh.y;
                }
                draw_image(box, _covers[slot]);
            }
            if (refresh.w) {
                _screen_has_images = true;
                M5.Display.display(refresh.x, refresh.y, refresh.w, refresh.h);
                ESP_LOGI(tag, "feed cover refresh area=%d,%d %dx%d",
                    refresh.x, refresh.y, refresh.w, refresh.h);
            }
            end();
        }
        _consumed = ready;
        if (done) {
            if (!canceled && !_media->error.empty())
                ESP_LOGW(tag, "%s image task: %s", _detail ? "detail" : "feed", _media->error.c_str());
            _media.reset();
        }
    }

    void open_note(const xhs::Card &card) {
        if (_data) return;
        _detail = true;
        _note_id = card.id;
        _placeholder_author = card.author;
        _placeholder_likes = card.likes;
        restart_images();
        start_data(Action::Detail);
    }

    void leave_detail() {
        cancel_tasks();
        _data.reset();
        _note = {};
        _album = {};
        _album_cache.clear();
        _error.clear();
        _note_id.clear();
        _placeholder_author.clear();
        _placeholder_likes.clear();
        _image_index = _text_page = 0;
        _article_pages.assign(1, {});
        _next_text = _detail = _loaded = _album_ready = false;
        _fullscreen = _album_full_size = _tap_pending = false;
        restart_images();
    }

    void toggle_fullscreen() {
        _tap_pending = false;
        _fullscreen = !_fullscreen;
        // Keep the currently decoded image visible while fetching the larger
        // rendition. A decoded full-size image also serves the smaller view.
        _album_ready = _album.pixels && (!_fullscreen || _album_full_size);
        restart_images();
        draw();
    }

    void image_tap(const TouchEvent &event) {
        const uint32_t now = lgfx::millis();
        if (_tap_pending && now - _tap_time <= double_tap_ms &&
            std::max(std::abs(event.x-_tap_x), std::abs(event.y-_tap_y)) <= double_tap_slop) {
            toggle_fullscreen();
            return;
        }
        _tap_pending = true;
        _tap_time = now;
        _tap_x = event.x;
        _tap_y = event.y;
        if (_album_ready && !_album.pixels) {
            _album_ready = false;
            restart_images();
            draw_album();
        }
    }

    void back() {
        if (_fullscreen) toggle_fullscreen();
        else if (_detail) { leave_detail(); draw(); }
        else ActivityManager::instance().pop();
    }

    void trim_album_cache() {
        const auto bytes = [](const xhs::Image &image) -> size_t {
            return image.pixels ? size_t((image.width+1)/2) * image.height : 0;
        };
        size_t used = bytes(_album);
        for (const auto &photo : _album_cache) used += bytes(photo.image);
        while (used > album_cache_budget && !_album_cache.empty()) {
            used -= bytes(_album_cache.front().image);
            _album_cache.erase(_album_cache.begin());
        }
    }

    void step_image(int direction) {
        const int next = int(_image_index)+direction;
        if (next < 0 || next >= int(_note.images.size())) return;

        // Take the requested image out before saving the current one, so an
        // about-to-be-viewed entry cannot be dropped by the memory limit.
        xhs::Image image;
        bool full_size = false;
        const auto cached = std::find_if(_album_cache.begin(), _album_cache.end(),
            [next](const CachedPhoto &photo) { return photo.index == size_t(next); });
        if (cached != _album_cache.end()) {
            image = std::move(cached->image);
            full_size = cached->full_size;
            _album_cache.erase(cached);
        }
        if (_album.pixels)
            _album_cache.push_back({_image_index, std::move(_album), _album_full_size});

        _image_index = next;
        _album = std::move(image);
        _album_full_size = full_size;
        _album_ready = _album.pixels && (!_fullscreen || _album_full_size);
        _tap_pending = false;
        if (_album.pixels) {
            ESP_LOGI(tag, "detail image cache hit index=%u full=%d upgrade=%d",
                unsigned(_image_index+1), _album_full_size, !_album_ready);
        }
        // Cancel the previous selection. start_images skips the fetch on a
        // cache hit, except when a gallery image needs a full-size upgrade.
        restart_images();
        draw_album();
    }

    void move(int direction) {
        if (_detail) {
            if (!_loaded || (direction > 0 ? !_next_text : _text_page == 0)) return;
            _text_page = size_t(int(_text_page)+direction);
            if (_text_page == 0 && !_album_ready) restart_images();
            draw();
            return;
        }
        if (!_config.ready() || _data) return;
        if (recoverable_session()) { reopen_list(); return; }
        if (_session.empty()) { start_data(Action::Open); return; }
        if (direction < 0 && !_page) { start_data(Action::Refresh); return; }
        if (direction > 0 && _page+xhs::batch_size >= _cards.size()) {
            start_data(Action::More);
            return;
        }
        _page = size_t(int(_page)+direction*int(xhs::batch_size));
        _error.clear();
        clear_covers();
        restart_images();
        draw();
    }

};

class Settings final : public StatusBarActivity {
public:
    void on_create() override { draw(); }
    void on_show() override { if (_shown) draw(); _shown = true; }
    ~Settings() override { if (_probe) _probe->cancel.store(true); }
    bool can_standby() const override { return !_probe || _probe->done.load(); }

    void loop() override {
        if (_probe && _probe->done.load(std::memory_order_acquire)) {
            _message = _probe->error.empty() ? TextId::XhsServiceReachable : error_text(_probe->error);
            _probe.reset();
            draw();
        }
        StatusBarActivity::loop();
    }

    bool on_touch(const TouchEvent &event) override {
        if (event.type == TouchEventType::Back) { ActivityManager::instance().pop(); return true; }
        if (event.type != TouchEventType::Tap) return false;
        if (status_bar_touch(event)) return true;
        if (event.y < start_y || event.y >= start_y+rows*row_h) return false;
        _focus = (event.y-start_y)/row_h;
        choose();
        return true;
    }

    bool on_key(int key) override {
        if (key == APP_KEY_BACK) { ActivityManager::instance().pop(); return true; }
        if (key == APP_KEY_OK) { choose(); return true; }
        if (key != APP_KEY_UP && key != APP_KEY_DOWN) return false;
        _focus = (_focus+rows+(key == APP_KEY_DOWN ? 1 : -1))%rows;
        draw();
        return true;
    }

private:
    static constexpr int start_y = header;
    static constexpr int row_h = 126, rows = 5;
    xhs::Config _draft = xhs::load_config();
    std::shared_ptr<xhs::Task> _probe;
    TextId _message = TextId::Count;
    int _focus = 0;
    bool _shown = false;

    void edit(bool token) {
        TextInputOptions options;
        options.title = text(token ? TextId::XhsDeviceToken : TextId::XhsServiceAddress);
        options.initial_text = token ? _draft.token : _draft.address;
        options.max_bytes = token ? xhs::token_limit : xhs::address_limit;
        options.secret = token;
        options.start_in_chinese = false;
        options.text_refresh = true;
        options.unified_status_bar = true;
        options.validate = [token](const std::string &value) -> std::string {
            if (value.empty() || (token ? xhs::valid_token(value) : xhs::valid_address(value))) return {};
            return text(token ? TextId::XhsInvalidToken : TextId::XhsInvalidAddress);
        };
        launch_text_input(options, [this, token](const std::string &value) {
            if (token) _draft.token = value;
            else {
                _draft.address = value;
                while (!_draft.address.empty() && _draft.address.back() == '/') _draft.address.pop_back();
            }
            _message = TextId::Count;
        });
    }

    void choose() {
        if (_probe) return;
        if (_focus < 2) { edit(_focus == 1); return; }
        if (_focus == 2) {
            _draft.image_mode = _draft.image_mode == xhs::ImageMode::Gray4 ? xhs::ImageMode::Cdn : xhs::ImageMode::Gray4;
            _message = TextId::Count;
        } else if (_focus == 3) {
            if (!xhs::valid_address(_draft.address)) _message = TextId::XhsInvalidAddress;
            else {
                auto job = std::make_shared<xhs::Task>(Action::Health);
                if (xhs::start(job, _draft, xhs::make_network())) {
                    _probe = std::move(job);
                    _message = TextId::XhsTesting;
                } else _message = TextId::XhsConnectionFailed;
            }
        } else {
            _message = xhs::save_config(_draft) ? TextId::XhsSaved : TextId::XhsSaveFailed;
        }
        draw();
    }

    void draw() {
        const int width = M5.Display.width(), px = system_font::pixel_size();
        const TextId labels[] = {TextId::XhsServiceAddress, TextId::XhsDeviceToken, TextId::XhsImageMode,
                                 TextId::XhsTestConnection, TextId::XhsSave};
        const std::string values[] = {_draft.address, _draft.token.empty() ? "" : "********",
            text(_draft.image_mode == xhs::ImageMode::Gray4 ? TextId::XhsNasGray : TextId::XhsCdn),
            {}, {}};

        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(text(TextId::Xiaohongshu));
        for (int row = 0; row < rows; ++row) {
            const int y = start_y+row*row_h;
            M5.Display.drawFastHLine(pad, y+row_h-1, width-2*pad, separator);
            list_focus::draw(M5.Display, pad, y, row_h, row == _focus);
            const int left = pad+list_focus::text_inset();
            system_font::draw_left_sized(M5.Display, text(labels[row]), left, y+18, px);
            if (!values[row].empty())
                system_font::draw_left_sized(M5.Display, app_status_bar::fit(values[row], width-left-pad, 24),
                                            left, y+70, 24, TFT_DARKGRAY);
        }
        if (_message != TextId::Count)
            centered(app_status_bar::fit(text(_message), width-2*pad, 26), width/2, start_y+rows*row_h+48, 26);
        M5.Display.display();
        M5.Display.endWrite();
    }
};

} // namespace

void launch_xhs_reader() { ActivityManager::instance().push(new Reader()); }
void launch_xhs_settings() { ActivityManager::instance().push(new Settings()); }
bool xhs_reader_configured() { return xhs::load_config().ready(); }
