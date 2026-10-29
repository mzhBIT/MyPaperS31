#include "ui/ControlCenter.h"
#include "activities/ActivityManager.h"
#include "frontlight/frontlight.h"
#include "ui/FrontlightControls.h"
#include "ui/BatteryStatus.h"
#include "ui/HeaderLayout.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include <M5Unified.h>
#include <lgfx/v1/platforms/esp32/Panel_EPD_2D.hpp>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <algorithm>
#include <cstdio>
#include <new>
#include <sdkconfig.h>
#if CONFIG_IDF_TARGET_ESP32S31
#include "boards/MyPaperS31/StandbyS31.h"
#endif

namespace {
bool pending = false;
uint32_t pending_generation = 0;

class ControlCenter final : public Activity {
    using Kind = frontlight_controls::Kind;
public:
    ~ControlCenter() override { save(); heap_caps_free(_snapshot); }
    bool capture() {
        auto* panel = static_cast<lgfx::Panel_EPD_2D*>(M5.Display.getPanel());
        const auto cfg = panel->config();
        _width = cfg.panel_width; _height = cfg.panel_height;
        _rotation = M5.Display.getRotation();
        const size_t bytes = size_t(cfg.memory_width) * cfg.memory_height / 2;
        _snapshot = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return _snapshot && panel->copyPhysicalGrayscale4(_snapshot, bytes);
    }
    void on_create() override {
        _sequence = touch_input_contact().sequence;
        _previous_mode = M5.Display.getEpdMode();
        _battery_level = read_battery();
        _free_psram_kb = read_free_psram();
        _free_sram_kb = read_free_sram();
        _last_battery_check = lgfx::millis();
        draw();
    }
    void on_overlay_dismiss(bool restore) override {
        save();
        if (restore && _snapshot) {
            M5.Display.waitDisplay();
            M5.Display.startWrite();
            if (M5.Display.getPanel()->writePhysicalGrayscale4(_snapshot, _width, _height, _rotation)) {
                // The covered page can contain gray covers/photos or text.
                M5.Display.setEpdMode(epd_mode_t::epd_hd);
                M5.Display.display(0, 0, M5.Display.width(), panel_height());
            }
            M5.Display.endWrite();
        }
        M5.Display.setEpdMode(_previous_mode);
        touch_input_cancel_until_release();
    }
    bool on_key(int key) override {
        if (key == APP_KEY_BACK || key == APP_KEY_OK) { _closing = true; return true; }
        if (key == APP_KEY_UP || key == APP_KEY_DOWN) {
            adjust(_focus, frontlight_controls::value(Kind(_focus)) + (key == APP_KEY_UP ? 1 : -1)*frontlight_controls::step(Kind(_focus)));
            save(); return true;
        }
        return true;
    }
    bool on_cover_home_key() override { _closing = true; return true; }
    bool on_navigation_key(int) override { return true; }
    bool allow_system_home() const override { return false; }
    bool on_touch(const TouchEvent& event) override {
        int row = _drag >= 0 ? _drag : slider_hit(event.start_x, event.start_y);
        if (row < 0 && event.type == TouchEventType::Tap) row = slider_hit(event.x, event.y);
        if (row >= 0) {
            preview(row, event.x);
            if (!touch_input_contact().down) { _drag = -1; save(); }
            return true;
        }
        if (event.type == TouchEventType::Back || event.type == TouchEventType::SwipeUp ||
            event.type == TouchEventType::SwipeUpLeft || event.type == TouchEventType::SwipeUpRight) {
            _closing = true; return true;
        }
        if (event.type == TouchEventType::Tap) {
            if (header_layout::right_hit(event.x, event.y, M5.Display.width(), 90)) {
#if CONFIG_IDF_TARGET_ESP32S31
                _shutdown = true;
#else
                _closing = true;
#endif
            } else if (event.y >= panel_height())
                _closing = true;
            else for (int row=0; row<rows(); ++row) {
                if (event.y < slider_y(row)-40 || event.y >= slider_y(row)+40) continue;
                const int delta = event.x < 90 ? -1 : event.x >= M5.Display.width()-90 ? 1 : 0;
                if (delta) adjust(row, frontlight_controls::value(Kind(row)) + delta*frontlight_controls::step(Kind(row)));
                save(); break;
            }
        }
        return true; // No overlay contact falls through to the covered app.
    }
    void loop() override {
        const auto contact = touch_input_contact();
        if (!_closing && contact.down && contact.sequence != _sequence) {
            _sequence = contact.sequence;
            _drag = slider_hit(contact.start_x, contact.start_y);
            if (_drag >= 0) touch_input_claim_contact();
        }
        if (_drag >= 0) {
            preview(_drag, contact.x);
            if (!contact.down) { _drag = -1; save(); }
        }
        if (M5.Display.displayBusy()) return;
#if CONFIG_IDF_TARGET_ESP32S31
        if (_shutdown) {
            // Dismiss first: an overlay prevents standby, and dismiss changes
            // the activity generation. Request against the resulting page.
            ActivityManager::instance().dismiss_overlay(true);
            mypaper::board::s31::standby::request_shutdown();
            return; // The overlay (this) has been deleted.
        }
#endif
        if (_closing) { ActivityManager::instance().pop(); return; }
        if (!contact.down && uint32_t(lgfx::millis()-_last_battery_check) >= 1000) {
            _last_battery_check = lgfx::millis();
            const int level = read_battery();
            if (level != _battery_level) { _battery_level = level; _dirty |= BATTERY_DIRTY; }
            const size_t free_psram = read_free_psram();
            const size_t free_sram = read_free_sram();
            if (free_psram != _free_psram_kb || free_sram != _free_sram_kb) {
                _free_psram_kb = free_psram;
                _free_sram_kb = free_sram;
                _dirty |= PSRAM_DIRTY;
            }
        }
        if (_dirty && (_drag < 0 || uint32_t(lgfx::millis()-_last_draw) >= 160)) {
            M5.Display.startWrite();
            M5.Display.setEpdMode(epd_mode_t::epd_text);
            int top=panel_height(), bottom=0;
            for (int row=0; row<rows(); ++row) if (_dirty & (1u<<row)) {
                draw_row(row); top=std::min(top,row_top(row)); bottom=std::max(bottom,row_top(row+1));
            }
            if (_dirty & BATTERY_DIRTY) {
                draw_battery(); top=std::min(top,battery_y()); bottom=std::max(bottom,battery_y()+battery_height());
            }
            if (_dirty & PSRAM_DIRTY) {
                draw_psram(); top=std::min(top,psram_y()); bottom=std::max(bottom,psram_y()+battery_height());
            }
            if (bottom > top) M5.Display.display(0, top, M5.Display.width(), bottom-top);
            M5.Display.endWrite();
            _dirty = false; _last_draw = lgfx::millis();
        }
    }
private:
    uint8_t* _snapshot = nullptr;
    uint16_t _width = 0, _height = 0;
    uint8_t _rotation = 0;
    epd_mode_t _previous_mode = epd_mode_t::epd_text;
    uint32_t _sequence = 0, _last_draw = 0;
    uint32_t _last_battery_check = 0;
    int _battery_level = -1;
    size_t _free_psram_kb = 0;
    size_t _free_sram_kb = 0;
    static constexpr unsigned BATTERY_DIRTY = 1u << 2;
    static constexpr unsigned PSRAM_DIRTY = 1u << 3;
    bool _unsaved = false, _closing = false;
    bool _shutdown = false;
    int _drag = -1, _focus = 0;
    unsigned _dirty = 0;
    static int rows() { return frontlight_controls::has_temperature() ? 2 : 1; }
    // Brightness has no endpoint captions; avoid reserving a second text line.
    static int row_height(int row) { return row == 0 ? system_font::line_height()+104 : 2*system_font::line_height()+88; }
    static int row_top(int row) {
        int y=header_layout::height;
        for (int i=0; i<row; ++i) y += row_height(i);
        return y;
    }
    static int slider_y(int row) { return row_top(row)+system_font::line_height()+38; }
    static int battery_y() { return row_top(rows())+8; }
    static int battery_height() { return system_font::line_height()+24; }
    static int psram_y() { return battery_y()+battery_height(); }
    static int panel_height() { return psram_y()+battery_height()+24; }
    static size_t read_free_psram() {
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    }
    static size_t read_free_sram() {
        return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
    }
    static int read_battery() {
        return battery_status::available() ? std::clamp<int>(M5.Power.getBatteryLevel(), 0, 100) : -1;
    }
    void draw_battery() {
        M5.Display.fillRect(0, battery_y(), M5.Display.width(), battery_height(), TFT_WHITE);
        system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::BatteryLevel), 24, battery_y()+8);
        const std::string label = _battery_level >= 0 ? std::to_string(_battery_level)+"%" : "--%";
        system_font::draw_right(M5.Display, label, M5.Display.width()-24, battery_y()+8);
    }
    void draw_psram() {
        M5.Display.fillRect(0, psram_y(), M5.Display.width(), battery_height(), TFT_WHITE);
        const std::string label = ui_locale::text(ui_locale::TextId::FreeMemory);
        const std::string value = std::to_string(_free_psram_kb)+"KB/"+std::to_string(_free_sram_kb)+"KB";
        int px=system_font::pixel_size();
        while(px>16) {
            const int measured=system_font::measure(label,px)+system_font::measure(value,px);
            const int available=M5.Display.width()-72;
            if(measured<=available) break;
            px=std::max(16,std::min(px-1,int(int64_t(px)*available/measured)-1));
        }
        const int y=psram_y()+(battery_height()-system_font::line_height(px))/2;
        system_font::draw_left_sized(M5.Display, label, 24, y, px);
        system_font::draw_right_sized(M5.Display, value, M5.Display.width()-24, y, px);
    }
    static int left() { return 110; }
    static int right() { return M5.Display.width()-110; }
    static int slider_hit(int x, int y) {
        if (x < left()-16 || x > right()+16) return -1;
        for (int row=0; row<rows(); ++row)
            if (y >= slider_y(row)-30 && y < slider_y(row)+30) return row;
        return -1;
    }
    void save() { if (_unsaved) { frontlight_save_settings(); _unsaved = false; } }
    void adjust(int row, int next) {
        next = std::clamp(next, 0, 1000);
        _focus = row;
        if (next == frontlight_controls::value(Kind(row))) return;
        frontlight_controls::preview(Kind(row), next);
        _dirty |= 1u << row; _unsaved = true;
    }
    void preview(int row, int x) { adjust(row, frontlight_controls::from_x(x,left(),right())); }
    static void draw_row(int row) {
        const int width = M5.Display.width(), y = slider_y(row), top = row_top(row);
        M5.Display.fillRect(0, top, width, row_height(row), TFT_WHITE);
        system_font::draw_left(M5.Display, frontlight_controls::title(Kind(row)), 24, top+12);
        system_font::draw_right(M5.Display, frontlight_controls::label(Kind(row)), width-24, top+12);
        frontlight_controls::draw_slider(M5.Display, Kind(row), left(), right(), y);
        system_font::draw_center(M5.Display, "-", 48, y-system_font::line_height()/2);
        system_font::draw_center(M5.Display, "+", width-48, y-system_font::line_height()/2);
        if (row == 1) {
            system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::WarmLight), left()-14, y+30);
            system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::CoolLight), right()+14, y+30);
        }
    }
    void draw() {
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillRect(0, 0, M5.Display.width(), panel_height(), TFT_WHITE);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::ControlCenter),
            M5.Display.width()/2, header_layout::text_y());
#if CONFIG_IDF_TARGET_ESP32S31
        // Vector power symbol: independent of the selected font/subset.
        const int cx = M5.Display.width()-45, cy = header_layout::height/2;
        for (int r=12; r<=14; ++r) M5.Display.drawCircle(cx, cy+2, r, TFT_BLACK);
        M5.Display.fillRect(cx-7, cy-14, 15, 13, TFT_WHITE);
        M5.Display.fillRoundRect(cx-1, cy-16, 3, 19, 1, TFT_BLACK);
#else
        // P4 has no implemented hardware power-cut path; do not substitute sleep.
        system_font::draw_right(M5.Display, "x", M5.Display.width()-28, header_layout::text_y());
#endif
        header_layout::draw_separator(M5.Display);
        for (int row=0; row<rows(); ++row) draw_row(row);
        draw_battery();
        draw_psram();
        M5.Display.drawFastHLine(0, panel_height()-1, M5.Display.width(), TFT_BLACK);
        M5.Display.fillRoundRect(M5.Display.width()/2-30, panel_height()-14, 60, 4, 2, TFT_BLACK);
        M5.Display.display(0, 0, M5.Display.width(), panel_height());
        M5.Display.endWrite();
        _dirty = false; _last_draw = lgfx::millis();
    }
};
}

bool control_center_handle_touch(const TouchEvent& event) {
    auto& manager = ActivityManager::instance();
    if (manager.has_overlay()) return false;
    if (pending) return true;
    if (event.start_y < 0 || event.start_y > 48 || event.dy < 64 || event.dy < std::abs(event.dx)) return false;
    if (event.type != TouchEventType::SwipeDown && event.type != TouchEventType::SwipeDownLeft &&
        event.type != TouchEventType::SwipeDownRight) return false;
    pending = true;
    pending_generation = manager.generation();
    touch_input_cancel_until_release();
    return true;
}

void control_center_service() {
    if (!pending) return;
    auto& manager = ActivityManager::instance();
    if (manager.generation() != pending_generation || manager.has_overlay()) { pending = false; return; }
    if (manager.is_blocked() || M5.Display.displayBusy()) return;
    pending = false;
    auto* center = new (std::nothrow) ControlCenter();
    if (!center || !center->capture() || !manager.show_overlay(center)) {
        delete center;
        ESP_LOGW("CONTROL_CENTER", "cannot open: snapshot unavailable");
    }
}
