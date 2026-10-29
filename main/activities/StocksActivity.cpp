#include "StocksActivity.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "stocks/StockClient.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/TextInputOverlay.h"
#include "ui/UiLocale.h"
#include "wifi/wifi_setting.h"
#include <M5Unified.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <memory>
#include <cstdio>
#include <algorithm>

namespace {
using ui_locale::TextId;
const char *tr(TextId id) { return ui_locale::text(id); }
constexpr int HEADER = header_layout::height, FOOTER = 112, ROWS = 5;
struct Request {
    std::vector<std::string> symbols;
    std::vector<stocks::Quote> quotes;
    std::string error;
    std::atomic<bool> done{false};
    bool success = false;
};
void fetch_task(void *arg)
{
    std::unique_ptr<std::shared_ptr<Request>> holder(static_cast<std::shared_ptr<Request> *>(arg));
    auto request = *holder;
    const auto err = wifi_setting_connect_saved(15000);
    if (err == ESP_OK) request->success = stocks::fetch(request->symbols, request->quotes, request->error);
    else request->error = esp_err_to_name(err);
    request->done.store(true, std::memory_order_release);
    // Destroy C++ owners before deleting the FreeRTOS task.
    request.reset(); holder.reset();
    vTaskDeleteWithCaps(nullptr);
}
std::string price(double v) { char b[40]; std::snprintf(b, sizeof(b), "%.2f", v); return b; }
std::string change(const stocks::Quote &q)
{
    char b[64];
    std::snprintf(b, sizeof(b), "%+.2f  (%+.2f%%)", q.last - q.previous, (q.last / q.previous - 1) * 100);
    return b;
}
class StocksActivity final : public Activity {
    std::vector<std::string> _symbols;
    std::vector<stocks::Quote> _quotes;
    std::shared_ptr<Request> _request;
    ui::TextInputOverlay _input;
    int _page = 0, _detail = -1, _remove = -1;
    bool _dirty = true, _exit = false;
    int64_t _last_request = -10000000;
    std::string _status, _pending_symbol;
    int row_height() const { return (M5.Display.height() - HEADER - FOOTER) / ROWS; }
    const stocks::Quote *quote(size_t i) const {
        return i < _quotes.size() && _quotes[i].symbol == _symbols[i] ? &_quotes[i] : nullptr;
    }
    void refresh() {
        if (_request || _symbols.empty()) return;
        const int64_t now = esp_timer_get_time();
        if (now - _last_request < 10000000) { _status = tr(TextId::StocksRefreshLimit); _dirty = true; return; }
        _last_request = now;
        _request = std::make_shared<Request>(); _request->symbols = _symbols;
        auto *context = new std::shared_ptr<Request>(_request);
        // connect_saved reads NVS credentials and Wi-Fi initialization may also
        // access flash. These operations can disable caches: a PSRAM task stack
        // is not safe here, even when the actual HTTP buffers use external RAM.
        if (xTaskCreatePinnedToCoreWithCaps(fetch_task, "stock_quotes", 12288, context, 1, nullptr, 0,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            delete context; _request.reset(); _status = tr(TextId::StocksRequestFailed);
        } else _status = tr(TextId::StocksLoading);
        _dirty = true;
    }
    void back() {
        if (_remove >= 0) { _remove = -1; _status.clear(); _dirty = true; }
        else if (_detail >= 0) { _detail = -1; _dirty = true; }
        else if (_request) { _exit = true; _status = tr(TextId::StocksFinishing); _dirty = true; }
        else ActivityManager::instance().pop();
    }
    void draw() {
        auto &d = M5.Display; const int w = d.width(), h = d.height();
        d.setEpdMode(epd_mode_t::epd_text); d.startWrite(); d.fillScreen(TFT_WHITE);
        system_font::draw_left(d, "<", 18, header_layout::text_y());
        system_font::draw_center(d, tr(TextId::Stocks), w / 2, header_layout::text_y());
        system_font::draw_right(d, tr(TextId::RefreshAction), w - 16, header_layout::text_y());
        header_layout::draw_separator(d);
        if (_detail >= 0) {
            const auto *q = quote(_detail);
            std::vector<std::string> lines = {_symbols[_detail], q && !q->name.empty() ? q->name : "",
                std::string(tr(TextId::StocksLast)) + ": " + (q && q->valid ? price(q->last) : "--")};
            if (q && q->valid) {
                lines.push_back(change(*q));
                lines.push_back(std::string(tr(TextId::StocksPrevious)) + ": " + price(q->previous));
                lines.push_back(std::string(tr(TextId::StocksOpen)) + ": " + price(q->open));
                lines.push_back(std::string(tr(TextId::StocksHighLow)) + ": " + price(q->high) + " / " + price(q->low));
                lines.push_back(std::string(tr(TextId::StocksCurrency)) + ": " + (_symbols[_detail].compare(0, 2, "us") == 0 ? "USD" : "CNY"));
                lines.push_back(tr(TextId::StocksSourceTime)); lines.push_back(q->time);
            }
            int y = HEADER + 18;
            for (const auto &line : lines) { if (y + system_font::line_height() > h - FOOTER) break;
                system_font::draw_left(d, system_font::ellipsize(line, w - 36), 18, y); y += system_font::line_height() + 14; }
        } else {
            for (int r = 0; r < ROWS; ++r) {
                const size_t i = _page * ROWS + r; if (i >= _symbols.size()) break;
                const int y = HEADER + r * row_height(); const auto *q = quote(i);
                system_font::draw_left(d, system_font::ellipsize(_symbols[i] + "  " + (q ? q->name : ""), w - 36), 18, y + 12);
                system_font::draw_left(d, q && q->valid ? price(q->last) + "  " + change(*q) : "--", 18, y + 64);
                d.drawFastHLine(18, y + row_height() - 1, w - 36, TFT_BLACK);
            }
            if (_symbols.empty()) system_font::draw_center(d, tr(TextId::StocksEmpty), w / 2, HEADER + 32);
        }
        d.drawFastHLine(0, h - FOOTER, w, TFT_BLACK);
        system_font::draw_left(d, system_font::ellipsize(_status.empty() ? tr(TextId::StocksDisclaimer) : _status, w - 32), 16, h - FOOTER + 6);
        system_font::draw_left(d, _remove >= 0 ? tr(TextId::Cancel) : tr(TextId::StocksAdd), 18, h - 52);
        system_font::draw_right(d, _remove >= 0 ? tr(TextId::Delete) :
            (_detail >= 0 ? tr(TextId::StocksRemove) : std::to_string(_page + 1) + "/" + std::to_string(std::max<size_t>(1, (_symbols.size() + ROWS - 1) / ROWS))), w - 18, h - 52);
        d.display(); d.endWrite(); _dirty = false;
    }
public:
    ~StocksActivity() override { wifi_setting_release(); }
    void on_create() override { _symbols = stocks::load_watchlist(); refresh(); }
    void on_show() override { _dirty = true; }
    bool allow_system_back() const override { return false; }
    bool can_standby() const override { return !_request; }
    void loop() override {
        if (_request && _request->done.load(std::memory_order_acquire)) {
            if (_request->success) {
                // Preserve previous quotes for individual symbols absent from a partial response.
                for (size_t i = 0; i < _request->quotes.size(); ++i)
                    if (!_request->quotes[i].valid && i < _quotes.size() && _quotes[i].symbol == _request->quotes[i].symbol)
                        _request->quotes[i] = _quotes[i];
                _quotes = std::move(_request->quotes); _status.clear();
            } else _status = std::string(tr(TextId::StocksRequestFailed)) + ": " + _request->error;
            _request.reset(); _dirty = true;
        }
        if (_exit && !_request) { ActivityManager::instance().pop(); return; }
        if (!_pending_symbol.empty() && !M5.Display.displayBusy()) {
            std::string symbol;
            if (!stocks::normalize_symbol(_pending_symbol, symbol)) _status = tr(TextId::StocksInvalid);
            else if (_symbols.size() >= 20) _status = tr(TextId::StocksLimit);
            else if (std::find(_symbols.begin(), _symbols.end(), symbol) == _symbols.end()) {
                auto next = _symbols; next.push_back(symbol);
                if (stocks::save_watchlist(next)) { _symbols = std::move(next); _status.clear(); _last_request = -10000000; refresh(); }
                else _status = tr(TextId::StocksSaveFailed);
            }
            _pending_symbol.clear(); _dirty = true;
        }
        if (_dirty && !_input.visible() && !M5.Display.displayBusy()) draw();
    }
    bool on_key(int key) override {
        if (_input.visible()) return _input.handle_key(key);
        if (key == APP_KEY_BACK) { back(); return true; }
        return false;
    }
    bool on_touch(const TouchEvent &e) override {
        if (_input.visible()) return _input.handle_touch(e);
        if (e.type == TouchEventType::Back || (e.type == TouchEventType::Tap && header_layout::left_hit(e.x, e.y, 100))) { back(); return true; }
        if (_exit || M5.Display.displayBusy()) return true;
        if (e.type == TouchEventType::Tap && header_layout::right_hit(e.x, e.y, M5.Display.width(), 180)) { refresh(); return true; }
        if (e.type == TouchEventType::SwipeUp || e.type == TouchEventType::SwipeDown) {
            if (_detail < 0 && _remove < 0) {
                _page = std::max(0, std::min(_page + (e.type == TouchEventType::SwipeUp ? 1 : -1), static_cast<int>((_symbols.size() ? _symbols.size() - 1 : 0) / ROWS)));
                _dirty = true;
            }
            return true;
        }
        if (e.type != TouchEventType::Tap && e.type != TouchEventType::LongPress) return false;
        if (e.y >= M5.Display.height() - 58) {
            if (_remove >= 0) {
                if (e.x > M5.Display.width() / 2 && !_request) {
                    auto next = _symbols; next.erase(next.begin() + _remove);
                    if (stocks::save_watchlist(next)) {
                        if (_remove < static_cast<int>(_quotes.size())) _quotes.erase(_quotes.begin() + _remove);
                        _symbols = std::move(next); _detail = -1; _page = 0; _status.clear();
                    }
                    else _status = tr(TextId::StocksSaveFailed);
                } else _status.clear();
                _remove = -1; _dirty = true;
            } else if (e.x < M5.Display.width() / 2 && !_request) {
                TextInputOptions options; options.title = tr(TextId::StocksSymbolHint); options.max_bytes = 18; options.start_in_chinese = false;
                _input.open(options, [this](const std::string &s) { _pending_symbol = s; _dirty = true; }, [this] { _dirty = true; });
            } else if (_detail >= 0 && !_request) { _remove = _detail; _status = _symbols[_remove] + " ?"; _dirty = true; }
            return true;
        }
        if (_detail < 0 && _remove < 0 && e.y >= HEADER && e.y < M5.Display.height() - FOOTER) {
            const int i = _page * ROWS + (e.y - HEADER) / row_height();
            if (i < static_cast<int>(_symbols.size())) {
                if (e.type == TouchEventType::LongPress && !_request) { _remove = i; _status = _symbols[i] + " ?"; }
                else _detail = i;
                _dirty = true;
            }
        }
        return true;
    }
};
}
void launch_stocks() { ActivityManager::instance().push(new StocksActivity()); }
