#include "activities/ZhihuActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RssReaderActivity.h"
#include "activities/SettingActivity.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include "zhihu/ZhihuClient.h"
#include "zhihu/ZhihuConfig.h"

#include <M5Unified.h>
#include <esp_log.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr const char *TAG = "ZHIHU";
constexpr int HEADER_H = 76;
constexpr int LIST_TOP = HEADER_H;
constexpr int FOOTER_H = 72;
constexpr int SIDE_PAD = 18;
constexpr int PAGE_SIZE = 5;

size_t utf8_char_bytes(const std::string &text, size_t position)
{
    const unsigned char c = static_cast<unsigned char>(text[position]);
    if ((c & 0xE0) == 0xC0) return std::min<size_t>(2, text.size() - position);
    if ((c & 0xF0) == 0xE0) return std::min<size_t>(3, text.size() - position);
    if ((c & 0xF8) == 0xF0) return std::min<size_t>(4, text.size() - position);
    return 1;
}

std::vector<std::string> wrap_text(const std::string &text, int width, size_t max_lines)
{
    std::vector<std::string> lines;
    std::string line;
    for (size_t position = 0; position < text.size() && lines.size() < max_lines;) {
        const size_t count = utf8_char_bytes(text, position);
        const std::string glyph = text.substr(position, count);
        if (!line.empty() && system_font::measure(line + glyph) > width) {
            lines.push_back(line);
            line.clear();
            if (lines.size() >= max_lines) break;
        }
        line += glyph;
        position += count;
    }
    if (!line.empty() && lines.size() < max_lines) lines.push_back(line);
    return lines;
}

void draw_header()
{
    const int width = M5.Display.width();
    M5.Display.fillRect(0, 0, width, HEADER_H, TFT_WHITE);
    system_font::draw_left(M5.Display, "<", 18, 12, TFT_BLACK);
    system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ZhihuHotList), width / 2, 12, TFT_BLACK);
    system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::RefreshAction), width - 16, 12, TFT_BLACK);
    M5.Display.drawFastHLine(0, HEADER_H - 1, width, TFT_BLACK);
}

class ZhihuHotListActivity final : public Activity {
public:
    void on_create() override { reload(false); }

    void on_show() override
    {
        if (!_seen_show) {
            _seen_show = true;
            return;
        }
        draw(false);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (_content_error_visible) {
            if (event.type == TouchEventType::Tap || event.type == TouchEventType::Back) {
                _content_error_visible = false;
                draw(false);
                return true;
            }
            return false;
        }
        if (event.type == TouchEventType::Back ||
            (event.type == TouchEventType::Tap && event.y < HEADER_H && event.x < 120)) {
            ActivityManager::instance().pop();
            return true;
        }
        if (event.type == TouchEventType::Tap && event.y < HEADER_H &&
            event.x > M5.Display.width() - 180) {
            reload(true);
            return true;
        }
        if (event.type == TouchEventType::SwipeUp) {
            change_page(1);
            return true;
        }
        if (event.type == TouchEventType::SwipeDown) {
            change_page(-1);
            return true;
        }
        if (event.type == TouchEventType::Tap && !_result.success) {
            reload(true);
            return true;
        }
        if (event.type == TouchEventType::Tap && _result.success && event.y >= LIST_TOP &&
            event.y < M5.Display.height() - FOOTER_H) {
            const int list_height = M5.Display.height() - LIST_TOP - FOOTER_H;
            const int row_height = list_height / PAGE_SIZE;
            const int index = _page * PAGE_SIZE + (event.y - LIST_TOP) / row_height;
            if (index >= 0 && index < static_cast<int>(_result.items.size())) {
                open_content(index);
                return true;
            }
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (_content_error_visible) {
            if (key == APP_KEY_BACK || key == APP_KEY_OK) {
                _content_error_visible = false;
                draw(false);
                return true;
            }
            return false;
        }
        if (key == APP_KEY_BACK) {
            ActivityManager::instance().pop();
            return true;
        }
        if (key == APP_KEY_UP) {
            change_page(-1);
            return true;
        }
        if (key == APP_KEY_DOWN) {
            change_page(1);
            return true;
        }
        if (key == APP_KEY_OK || key == APP_KEY_EXTRA) {
            reload(true);
            return true;
        }
        return false;
    }

private:
    ZhihuHotListResult _result;
    int _page = 0;
    bool _seen_show = false;
    bool _content_error_visible = false;
    std::string _content_error;

    int page_count() const
    {
        return std::max(1, (static_cast<int>(_result.items.size()) + PAGE_SIZE - 1) / PAGE_SIZE);
    }

    void change_page(int delta)
    {
        const int target = std::max(0, std::min(page_count() - 1, _page + delta));
        if (target == _page) return;
        _page = target;
        draw(false);
    }

    void reload(bool force_network)
    {
        draw_status(ui_locale::text(force_network ? ui_locale::TextId::RefreshZhihuHotList
                                                  : ui_locale::TextId::LoadingZhihuHotList));
        M5.Display.waitDisplay();
        ZhihuConfig config;
        zhihu_config_load(&config);
        ZhihuHotListResult loaded = ZhihuClient::fetch_hot_list(config, 30, force_network);
        if (!loaded.success && _result.success) {
            ESP_LOGW(TAG, "refresh failed; retaining cached/current list: %s",
                     loaded.error.c_str());
        } else {
            _result = std::move(loaded);
            _page = 0;
        }
        ESP_LOGI(TAG, "hot list loaded success=%d cache=%d items=%u status=%d",
                 _result.success, _result.from_cache,
                 static_cast<unsigned>(_result.items.size()), _result.http_status);
        draw(true);
    }

    void open_content(int index)
    {
        const ZhihuHotItem &item = _result.items[index];
        if (item.summary.empty()) {
            _content_error = ui_locale::text(ui_locale::TextId::NoReadableSummary);
            _content_error_visible = true;
            draw_content_error();
            return;
        }
        ESP_LOGI(TAG, "open hot-list summary index=%d bytes=%u", index + 1,
                 static_cast<unsigned>(item.summary.size()));
        launch_rss_article(item.title, item.summary, item.url);
    }

    void draw_status(const char *message, bool quality = true)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();
        system_font::draw_center(M5.Display, message, M5.Display.width() / 2,
                                 M5.Display.height() / 2 - 24, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_content_error()
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();
        system_font::draw_center(M5.Display,
                                 system_font::ellipsize(_content_error,
                                                        M5.Display.width() - 50),
                                 M5.Display.width() / 2, 390, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ReturnToHotList),
                                 M5.Display.width() / 2, 485, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    void draw_item(int slot, int index)
    {
        const int width = M5.Display.width();
        const int list_height = M5.Display.height() - LIST_TOP - FOOTER_H;
        const int row_height = list_height / PAGE_SIZE;
        const int y = LIST_TOP + slot * row_height;
        const ZhihuHotItem &item = _result.items[index];
        const int rank_width = 54;
        const int text_x = SIDE_PAD + rank_width;
        const int text_width = width - text_x - SIDE_PAD;
        system_font::draw_left(M5.Display, std::to_string(index + 1), SIDE_PAD, y + 14,
                               TFT_BLACK);

        const int line_height = system_font::line_height();
        std::vector<std::string> title_lines = wrap_text(item.title, text_width, 2);
        int line_y = y + 10;
        for (const std::string &line : title_lines) {
            system_font::draw_left(M5.Display, line, text_x, line_y, TFT_BLACK);
            line_y += line_height;
        }
        if (!item.summary.empty()) {
            const int summary_y = std::min(y + row_height - line_height - 12,
                                           y + 10 + line_height * 2 + 8);
            system_font::draw_left(M5.Display,
                                   system_font::ellipsize(item.summary, text_width),
                                   text_x, summary_y, TFT_DARKGREY);
        }
        M5.Display.drawFastHLine(SIDE_PAD, y + row_height - 1,
                                 width - SIDE_PAD * 2, TFT_BLACK);
    }

    void draw(bool quality)
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();
        if (!_result.success) {
            const std::string message = _result.error.empty() ? ui_locale::text(ui_locale::TextId::HotListLoadFailed)
                                                               : ui_locale::localize_message(_result.error);
            system_font::draw_center(M5.Display,
                                     system_font::ellipsize(message, M5.Display.width() - 50),
                                     M5.Display.width() / 2, 390, TFT_BLACK);
            system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToRetry),
                                     M5.Display.width() / 2, 485, TFT_BLACK);
        } else {
            const int first = _page * PAGE_SIZE;
            for (int slot = 0; slot < PAGE_SIZE && first + slot < static_cast<int>(_result.items.size());
                 ++slot) {
                draw_item(slot, first + slot);
            }
            const std::string page = std::to_string(_page + 1) + " / " +
                                     std::to_string(page_count());
            system_font::draw_center(M5.Display, page, M5.Display.width() / 2,
                                     M5.Display.height() - FOOTER_H + 10, TFT_BLACK);
        }
        M5.Display.display();
        M5.Display.endWrite();
    }
};

class ZhihuSetupGuideActivity final : public Activity {
public:
    void on_create() override
    {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header();
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ZhihuSecretNotConfigured),
                                 M5.Display.width() / 2, 360, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::OpenZhihuSettingsHint),
                                 M5.Display.width() / 2, 450, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToReturn),
                                 M5.Display.width() / 2, 550, TFT_BLACK);
        M5.Display.display();
        M5.Display.endWrite();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Tap || event.type == TouchEventType::Back) {
            ActivityManager::instance().pop();
            return true;
        }
        return false;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK || key == APP_KEY_OK) {
            ActivityManager::instance().pop();
            return true;
        }
        return false;
    }
};

}  // namespace

void launch_zhihu()
{
    if (!ensure_wifi_configured_or_show_guide()) return;
    if (!zhihu_config_has_access_secret()) {
        ActivityManager::instance().push(new ZhihuSetupGuideActivity());
        return;
    }
    ActivityManager::instance().push(new ZhihuHotListActivity());
}
