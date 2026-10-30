#include "activities/EpdDiagnosticActivity.h"
#include "ui/ListFocus.h"

#include "activities/StatusBarActivity.h"
#include "activities/ActivityManager.h"
#include "ui/SystemFont.h"
#include "ui/HeaderLayout.h"
#include "ui/UiLocale.h"
#include "waveforms/my_waveform.h"

#include <M5Unified.h>
#include <esp_log.h>

#include "lgfx/v1/platforms/esp32/MyPaper_EPD_Config.hpp"
#include "lgfx/v1/platforms/esp32/Panel_EPD_2D.hpp"
#include "lgfx/v1/platforms/esp32/EpdScan2DContinuous.hpp"

#include <algorithm>
#include <cstdio>

namespace {

constexpr int HEADER_H = app_status_bar::height;
constexpr int PAD = 20;
constexpr int MENU_TOP = HEADER_H;
constexpr int MENU_ROW_H = 70;
constexpr int MENU_ROW_GAP = 4;
#if CONFIG_IDF_TARGET_ESP32S31
constexpr int MENU_COUNT = 14;
#else
constexpr int MENU_COUNT = 13;
#endif

const char *TAG = "EPD_DIAG";

const ui_locale::TextId MENU_LABELS[MENU_COUNT] = {
    ui_locale::TextId::DirectionPixelOrder,
    ui_locale::TextId::Grayscale16,
    ui_locale::TextId::WhiteToGray16,
    ui_locale::TextId::BlackToGray16,
    ui_locale::TextId::Transitions16x16,
    ui_locale::TextId::QualityBlackToWhite,
    ui_locale::TextId::TextDu4FourGray,
    ui_locale::TextId::FastestA2Binary,
    ui_locale::TextId::QualityTimingAb,
    ui_locale::TextId::QualityGc16Ab,
    ui_locale::TextId::QualityBlackHold,
    ui_locale::TextId::Du4NeutralMaskPartial,
    ui_locale::TextId::VcomMeasurement,
#if CONFIG_IDF_TARGET_ESP32S31
    ui_locale::TextId::OeGateTest,
#endif
};

void draw_center_fit(const char *text, int y, uint16_t color = TFT_BLACK)
{
    const int width = M5.Display.width();
    system_font::draw_center(M5.Display,
                             system_font::ellipsize(text, width - 24),
                             width / 2, y, color);
}

uint32_t gray_color(unsigned level)
{
    // Panel_EPD quantizes Quality pixels with (value - 8) >> 4.  Use the
    // center of each 16-value bucket so labels 0..15 map exactly to native
    // framebuffer levels 0..15 without the former 0/1 duplication.
    const uint8_t value = static_cast<uint8_t>((std::min(15u, level) << 4) + 8u);
    return M5.Display.color888(value, value, value);
}

void begin_draw(epd_mode_t mode)
{
    M5.Display.startWrite();
    M5.Display.setDither(false);
    M5.Display.setEpdMode(mode);
}

void finish_draw(bool wait = true)
{
    M5.Display.display();
    M5.Display.endWrite();
    if (wait) M5.Display.waitDisplay();
}

void finish_draw_area(int x, int y, int w, int h, bool wait = true)
{
    M5.Display.display(x, y, w, h);
    M5.Display.endWrite();
    if (wait) M5.Display.waitDisplay();
}

void draw_vertical_stripes(int x, int y, int w, int h, int stripe)
{
    M5.Display.fillRect(x, y, w, h, TFT_WHITE);
    for (int px = 0; px < w; px += stripe * 2) {
        M5.Display.fillRect(x + px, y, std::min(stripe, w - px), h, TFT_BLACK);
    }
    M5.Display.drawRect(x, y, w, h, TFT_BLACK);
}

void draw_horizontal_stripes(int x, int y, int w, int h, int stripe)
{
    M5.Display.fillRect(x, y, w, h, TFT_WHITE);
    for (int py = 0; py < h; py += stripe * 2) {
        M5.Display.fillRect(x, y + py, w, std::min(stripe, h - py), TFT_BLACK);
    }
    M5.Display.drawRect(x, y, w, h, TFT_BLACK);
}

class EpdDiagnosticActivity final : public StatusBarActivity {
public:
    ~EpdDiagnosticActivity() override
    {
        stop_oe_test();
        restore_default_timing();
    }

    void on_create() override
    {
        draw_menu(true);
    }

    void loop() override {
        // Do not insert status refreshes into waveform/timing test patterns.
        if (_view == View::Menu) StatusBarActivity::loop();
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (status_bar_touch(event)) return true;
        if (event.type == TouchEventType::Back) {
            return go_back();
        }
        if (event.type != TouchEventType::Tap) return false;

        if (_view == View::Menu) {
            for (int i = 0; i < MENU_COUNT; ++i) {
                const int top = MENU_TOP + i * (MENU_ROW_H + MENU_ROW_GAP);
                if (event.y >= top && event.y < top + MENU_ROW_H) {
                    _focus = i;
                    run_selected();
                    return true;
                }
            }
            return false;
        }

        if (_view == View::MatrixOld) {
            draw_matrix_target();
            return true;
        }
        if (_view == View::GhostSource) {
            draw_ghost_target();
            return true;
        }
        if (_view == View::Text4GrayBase) {
            draw_text_four_gray_target();
            return true;
        }
        if (_view == View::QualityTimingAB) {
            _timing_line_us = (_timing_line_us == 20u) ? 16u : 20u;
            draw_quality_timing_ab();
            return true;
        }
        if (_view == View::QualityWaveformAB) {
            _quality_36 = !_quality_36;
            draw_quality_waveform_ab();
            return true;
        }
        if (_view == View::BlackHoldWhite || _view == View::BlackHoldBlack) {
            draw_black_hold_black();
            return true;
        }
        if (_view == View::NeutralMaskSource || _view == View::NeutralMaskTarget) {
            draw_neutral_mask_partial();
            return true;
        }
        if (_view == View::OeGate) {
            if (event.y >= M5.Display.height() - 160) {
                _oe_gated = !_oe_gated;
                draw_oe_reference();
            } else {
                draw_oe_partial();
            }
            return true;
        }

        draw_menu(false);
        return true;
    }

    bool on_key(int key) override
    {
        if (key == APP_KEY_BACK) return go_back();
        if (_view == View::Menu) {
            if (key == APP_KEY_UP) {
                _focus = (_focus + MENU_COUNT - 1) % MENU_COUNT;
                draw_menu(false);
                return true;
            }
            if (key == APP_KEY_DOWN) {
                _focus = (_focus + 1) % MENU_COUNT;
                draw_menu(false);
                return true;
            }
            if (key == APP_KEY_OK) {
                run_selected();
                return true;
            }
            return false;
        }

        if (_view == View::MatrixOld && key == APP_KEY_OK) {
            draw_matrix_target();
            return true;
        }
        if (_view == View::GhostSource && key == APP_KEY_OK) {
            draw_ghost_target();
            return true;
        }
        if (_view == View::Text4GrayBase && key == APP_KEY_OK) {
            draw_text_four_gray_target();
            return true;
        }
        if (_view == View::QualityTimingAB && key == APP_KEY_OK) {
            _timing_line_us = (_timing_line_us == 20u) ? 16u : 20u;
            draw_quality_timing_ab();
            return true;
        }
        if (_view == View::QualityWaveformAB && key == APP_KEY_OK) {
            _quality_36 = !_quality_36;
            draw_quality_waveform_ab();
            return true;
        }
        if ((_view == View::BlackHoldWhite || _view == View::BlackHoldBlack)
         && key == APP_KEY_OK) {
            draw_black_hold_black();
            return true;
        }
        if ((_view == View::NeutralMaskSource || _view == View::NeutralMaskTarget)
         && key == APP_KEY_OK) {
            draw_neutral_mask_partial();
            return true;
        }
        if (_view == View::OeGate && (key == APP_KEY_OK || key == APP_KEY_EXTRA)) {
            if (key == APP_KEY_EXTRA) { _oe_gated = !_oe_gated; draw_oe_reference(); }
            else draw_oe_partial();
            return true;
        }
        if (key == APP_KEY_OK || key == APP_KEY_EXTRA) {
            draw_menu(false);
            return true;
        }
        return false;
    }

private:
    enum class View {
        Menu,
        Direction,
        Grayscale,
        MatrixOld,
        MatrixTarget,
        GhostSource,
        GhostTarget,
        Text4GrayBase,
        Text4Gray,
        A2Binary,
        QualityTimingAB,
        QualityWaveformAB,
        BlackHoldWhite,
        BlackHoldBlack,
        NeutralMaskSource,
        NeutralMaskTarget,
        Vcom,
        OeGate,
    };

    View _view = View::Menu;
    int _focus = 0;
    bool _neutral_mask_dark = true;
    uint32_t _timing_line_us = MYPAPER_EPD_2D_LINE_PERIOD_US;
    bool _quality_36 = false;
    unsigned _black_hold_refreshes = 0;
    bool _oe_gated = false, _oe_dark = true;
    unsigned _oe_refreshes = 0;

    void stop_oe_test()
    {
#if CONFIG_IDF_TARGET_ESP32S31
        if (_view == View::OeGate) {
            M5.Display.waitDisplay();
            lgfx::epd2d_continuous_scan_set_oe_test_window(0, 0);
            lgfx::epd2d_continuous_scan_set_oe_automatic(MYPAPER_EPD_S31_PARTIAL_OE_DEFAULT);
        }
#endif
    }

    void restore_default_timing()
    {
#if MYPAPER_EPD_USE_2D_ENGINE
        if (_timing_line_us != MYPAPER_EPD_2D_LINE_PERIOD_US) {
            if (lgfx::epd2d_continuous_scan_set_line_period_us(
                    MYPAPER_EPD_2D_LINE_PERIOD_US)) {
                ESP_LOGI(TAG, "timing A/B: restored default %u us",
                         (unsigned)MYPAPER_EPD_2D_LINE_PERIOD_US);
            }
            _timing_line_us = MYPAPER_EPD_2D_LINE_PERIOD_US;
        }
#endif
    }

    bool go_back()
    {
        if (_view == View::Menu) {
            ActivityManager::instance().pop();
        } else {
            draw_menu(false);
        }
        return true;
    }

    void draw_menu(bool quality)
    {
        if (_view == View::OeGate) {
            stop_oe_test();
            quality = true; // Remove any test ghosts before returning to UI.
        }
        if (_view == View::QualityTimingAB) {
            restore_default_timing();
        }
        _view = View::Menu;
        begin_draw(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::EpdDiagnostics));

        const int width = M5.Display.width();
        for (int i = 0; i < MENU_COUNT; ++i) {
            const int top = MENU_TOP + i * (MENU_ROW_H + MENU_ROW_GAP);
            const bool selected = i == _focus;
            constexpr uint16_t foreground = TFT_BLACK;
            list_focus::draw(M5.Display, PAD, top, MENU_ROW_H, selected);
            M5.Display.drawFastHLine(PAD, top + MENU_ROW_H - 1, width - PAD * 2, TFT_BLACK);
            system_font::draw_left(M5.Display, ui_locale::text(MENU_LABELS[i]), PAD + list_focus::text_inset(),
                                   top + std::max(0, (MENU_ROW_H - system_font::line_height()) / 2),
                                   foreground);
            system_font::draw_right(M5.Display, ">", width - PAD - 20,
                                    top + std::max(0, (MENU_ROW_H - system_font::line_height()) / 2),
                                    foreground);
        }

        draw_center_fit(ui_locale::text(ui_locale::TextId::EpdDiagModesHint),
                        M5.Display.height() - 100);
        finish_draw();
    }

    void run_selected()
    {
        switch (_focus) {
            case 0: draw_direction_pattern(); break;
            case 1: draw_grayscale_pattern(ui_locale::text(ui_locale::TextId::DirectGray16Caption)); break;
            case 2: draw_grayscale_from(15, ui_locale::text(ui_locale::TextId::WhiteGray16Caption)); break;
            case 3: draw_grayscale_from(0, ui_locale::text(ui_locale::TextId::BlackGray16Caption)); break;
            case 4: draw_matrix_old(); break;
            case 5: draw_ghost_source(); break;
            case 6: draw_text_four_gray_base(); break;
            case 7: draw_a2_binary(); break;
            case 8: draw_quality_timing_ab(); break;
            case 9: _quality_36 = false; draw_quality_waveform_ab(); break;
            case 10: draw_black_hold_white(); break;
            case 11: draw_neutral_mask_source(); break;
            case 12: run_vcom_measurement(); break;
#if CONFIG_IDF_TARGET_ESP32S31
            case 13: _oe_gated = false; draw_oe_reference(); break;
#endif
            default: break;
        }
    }

    struct TestRect {
        int x;
        int y;
        int w;
        int h;
    };

    TestRect oe_rect() const
    {
        const int width = M5.Display.width();
        return {width/3, 220, width/3, M5.Display.height()-440};
    }

    void draw_oe_reference()
    {
#if CONFIG_IDF_TARGET_ESP32S31
        M5.Display.waitDisplay();
        lgfx::epd2d_continuous_scan_set_oe_automatic(false);
        lgfx::epd2d_continuous_scan_set_oe_test_window(0, 0);
        _view = View::OeGate;
        _oe_dark = true; _oe_refreshes = 0;
        const auto r = oe_rect();
        const int width = M5.Display.width(), height = M5.Display.height();
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::OeGateTest));
        draw_center_fit(ui_locale::text(_oe_gated ? ui_locale::TextId::OeGateB : ui_locale::TextId::OeGateA), 100);
        // Left/right portrait references lie OUTSIDE the gated physical rows.
        // Top/bottom references lie on the SAME rows: OE cannot isolate them.
        for (int y = 190, n = 0; y < height-190; y += 52, ++n) {
            const auto color = gray_color((n % 4) * 5);
            M5.Display.fillRect(24, y, r.x-48, 34, color);
            M5.Display.fillRect(r.x+r.w+24, y, width-(r.x+r.w)-48, 34, color);
        }
        M5.Display.fillRect(r.x, 166, r.w, 24, TFT_BLACK);
        M5.Display.fillRect(r.x, height-190, r.w, 24, TFT_BLACK);
        M5.Display.fillRect(r.x, r.y, r.w, r.h, TFT_BLACK);
        M5.Display.drawRect(r.x-2, r.y-2, r.w+4, r.h+4, TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::OeGateRefreshHint), height-130);
        draw_center_fit(ui_locale::text(ui_locale::TextId::OeGateSwitchHint), height-78);
        finish_draw();
        if (_oe_gated) {
            // Only this symmetric portrait test geometry is supported. Its
            // central x interval maps to physical y regardless of mirror.
            if (width != 684 || height != 1216 || M5.Display.getRotation() != 0
                || !lgfx::epd2d_continuous_scan_set_oe_test_window(r.x, r.x+r.w)) {
                _oe_gated = false;
                lgfx::epd2d_continuous_scan_set_oe_automatic(MYPAPER_EPD_S31_PARTIAL_OE_DEFAULT);
                begin_draw(epd_mode_t::epd_quality);
                M5.Display.fillScreen(TFT_WHITE);
                draw_header(ui_locale::text(ui_locale::TextId::OeGateTest));
                draw_center_fit(ui_locale::text(ui_locale::TextId::OeGateUnavailable), height/2);
                finish_draw();
                _view = View::Grayscale;
                return;
            }
        }
        ESP_LOGI(TAG, "OE A/B reference: mode=%c physical_rows=228..456; tap refresh, bottom switch",
                 _oe_gated ? 'B' : 'A');
#endif
    }

    void draw_oe_partial()
    {
#if CONFIG_IDF_TARGET_ESP32S31
        const auto r = oe_rect();
        _oe_dark = !_oe_dark;
        begin_draw(epd_mode_t::epd_text);
        M5.Display.fillRect(r.x, r.y, r.w, r.h, _oe_dark ? TFT_BLACK : TFT_WHITE);
        M5.Display.drawRect(r.x+24, r.y+24, r.w-48, r.h-48, _oe_dark ? TFT_WHITE : TFT_BLACK);
        finish_draw_area(r.x, r.y, r.w, r.h);
        ESP_LOGI(TAG, "OE A/B partial complete: mode=%c refreshes=%u target=%s; inspect left/right references",
                 _oe_gated ? 'B' : 'A', ++_oe_refreshes, _oe_dark ? "black" : "white");
#endif
    }

    TestRect neutral_mask_rect() const
    {
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        return {width / 4, height / 4, width / 2, height / 2};
    }

    void draw_neutral_mask_outer_pattern()
    {
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        const TestRect r = neutral_mask_rect();
        draw_header(ui_locale::text(ui_locale::TextId::Du4NeutralMaskPartial));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::OuterPatternMustNotFade),
                                 width / 2, 102, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToggleCenter),
                                 width / 2, height - 74, TFT_BLACK);

        // These high-contrast marks are deliberately outside the requested
        // update rectangle. During each Text/DU4 partial refresh they must be
        // scanned with 00 (idle) drive values and retain their original ink.
        for (int y = 142; y < height - 130; y += 30) {
            M5.Display.fillRect(28, y, r.x - 56, 12, TFT_BLACK);
            M5.Display.fillRect(r.x + r.w + 28, y, width - (r.x + r.w) - 56,
                                12, TFT_BLACK);
        }
        for (int x = 34; x < width - 34; x += 46) {
            M5.Display.drawFastVLine(x, 132, r.y - 152, TFT_BLACK);
            M5.Display.drawFastVLine(x, r.y + r.h + 20,
                                     height - (r.y + r.h) - 130, TFT_BLACK);
        }
        M5.Display.drawRect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, TFT_BLACK);
    }

    void draw_neutral_mask_source()
    {
        _view = View::NeutralMaskSource;
        _neutral_mask_dark = true;
        const TestRect r = neutral_mask_rect();
        ESP_LOGI(TAG, "neutral-mask: establish outer reference Quality begin");
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        draw_neutral_mask_outer_pattern();
        M5.Display.fillRect(r.x, r.y, r.w, r.h, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::CenterBlack), r.x + r.w / 2,
                                 r.y + r.h / 2 - 24, TFT_WHITE);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapForDu4Partial), r.x + r.w / 2,
                                 r.y + r.h / 2 + 22, TFT_WHITE);
        finish_draw();
        ESP_LOGI(TAG, "neutral-mask: reference complete; tap for partial DU4");
    }

    void draw_neutral_mask_partial()
    {
        _view = View::NeutralMaskTarget;
        const TestRect r = neutral_mask_rect();
        _neutral_mask_dark = !_neutral_mask_dark;

        // Only the centre rectangle is written and submitted. Panel_EPD_2D
        // still scans every row, but it clears every non-centre output byte to
        // 00, the panel's idle drive. Repeated taps make any outer ink loss
        // immediately visible without changing the reference framebuffer.
        ESP_LOGI(TAG,
                 "neutral-mask: DU4 partial %s area=%d,%d %dx%d; exterior=00 idle",
                 _neutral_mask_dark ? "black" : "white", r.x, r.y, r.w, r.h);
        begin_draw(epd_mode_t::epd_text);
        M5.Display.fillRect(r.x, r.y, r.w, r.h,
                             _neutral_mask_dark ? TFT_BLACK : TFT_WHITE);
        M5.Display.drawRect(r.x + 20, r.y + 20, r.w - 40, r.h - 40,
                             _neutral_mask_dark ? TFT_WHITE : TFT_BLACK);
        const uint16_t text_color = _neutral_mask_dark ? TFT_WHITE : TFT_BLACK;
        system_font::draw_center(M5.Display,
                                 ui_locale::text(_neutral_mask_dark ? ui_locale::TextId::CenterBlack
                                                                     : ui_locale::TextId::CenterWhite),
                                 r.x + r.w / 2, r.y + r.h / 2 - 24, text_color);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToToggle), r.x + r.w / 2,
                                 r.y + r.h / 2 + 22, text_color);
        finish_draw_area(r.x, r.y, r.w, r.h);
        ESP_LOGI(TAG, "neutral-mask: partial DU4 complete; inspect outer reference");
    }

    void draw_text_four_gray_base()
    {
        _view = View::Text4GrayBase;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int header_h = 96;
        constexpr int footer_h = 116;
        const int pattern_top = header_h;
        const int pattern_bottom = height - footer_h;
        const int split_y = pattern_top + (pattern_bottom - pattern_top) / 2;

        // Pause after GC16 so its residual image can be inspected separately
        // from the subsequent DU4 transition.
        ESP_LOGI(TAG, "text four-gray: establish black/white split Quality begin");
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.fillRect(0, pattern_top, width, split_y - pattern_top,
                            TFT_BLACK);
        system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::QualityBaseTopBlack), 12, 18,
                               TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::InspectHalvesBeforeDu4), pattern_bottom + 16);
        draw_center_fit(ui_locale::text(ui_locale::TextId::TapShowFourGray), pattern_bottom + 58);
        finish_draw();
        ESP_LOGI(TAG, "text four-gray: Quality base complete; waiting for DU4 tap");
    }

    void draw_text_four_gray_target()
    {
        _view = View::Text4Gray;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int header_h = 96;
        constexpr int footer_h = 116;
        const int pattern_top = header_h;
        const int pattern_bottom = height - footer_h;
        const int pattern_h = pattern_bottom - pattern_top;

        begin_draw(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        const unsigned levels[4] = {0, 5, 10, 15};
        for (int column = 0; column < 4; ++column) {
            const int x0 = column * width / 4;
            const int x1 = (column + 1) * width / 4;
            M5.Display.fillRect(x0, pattern_top, x1 - x0, pattern_h,
                                gray_color(levels[column]));

            char label[12] = {};
            snprintf(label, sizeof(label), "%u", levels[column]);
            const uint16_t label_color = column < 2 ? TFT_WHITE : TFT_BLACK;
            system_font::draw_center(M5.Display, label, (x0 + x1) / 2,
                                     pattern_top + 18, label_color);
        }
        system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::Gray0Black), 12, 18, TFT_BLACK);
        system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::Gray15White), width - 12, 18,
                                TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::Du4ColumnsUniform), pattern_bottom + 16);
        draw_center_fit(ui_locale::text(ui_locale::TextId::TopBlackBottomWhite), pattern_bottom + 58);
        ESP_LOGI(TAG, "text four-gray: epd_text refresh begin");
        finish_draw();
        ESP_LOGI(TAG, "text four-gray: complete");
    }

    void draw_a2_binary()
    {
        _view = View::A2Binary;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int header_h = 96;
        constexpr int footer_h = 116;
        const int pattern_top = header_h;
        const int pattern_bottom = height - footer_h;
        const int pattern_h = pattern_bottom - pattern_top;
        const int split_y = pattern_top + pattern_h / 2;

        // As with the DU4 diagnostic, start the upper and lower halves from
        // opposite physical endpoints. Every final column must be uniform
        // across the old-state boundary after the fourteen-frame A2 update.
        ESP_LOGI(TAG, "A2 binary: establish white/black split Quality begin");
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.fillRect(0, split_y, width, height - split_y, TFT_BLACK);
        finish_draw();
        ESP_LOGI(TAG, "A2 binary: base split complete");

        begin_draw(epd_mode_t::epd_fastest);
        M5.Display.fillScreen(TFT_WHITE);
        for (int column = 0; column < 4; ++column) {
            const int x0 = column * width / 4;
            const int x1 = (column + 1) * width / 4;
            if ((column & 1) == 0) {
                M5.Display.fillRect(x0, pattern_top, x1 - x0,
                                    pattern_h, TFT_BLACK);
            }
        }
        system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::BlackLabel), 12, 18, TFT_BLACK);
        system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::WhiteLabel), width - 12, 18,
                                TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::A2AlternatingColumns),
                        pattern_bottom + 16);
        draw_center_fit(ui_locale::text(ui_locale::TextId::NoSeamBetweenHalves),
                        pattern_bottom + 58);
        ESP_LOGI(TAG, "A2 binary: epd_fastest refresh begin");
        finish_draw();
        ESP_LOGI(TAG, "A2 binary: complete");
    }

    void draw_quality_timing_ab()
    {
        _view = View::QualityTimingAB;
        // Each run recreates the split physical source at the selected scan
        // period.  This prevents A and B from inheriting one another's ink
        // state while leaving both the Quality LUT and its phase count intact.
        const uint32_t requested = _timing_line_us;
#if MYPAPER_EPD_USE_2D_ENGINE
        if (!lgfx::epd2d_continuous_scan_set_line_period_us(requested)) {
            ESP_LOGE(TAG, "timing A/B: cannot select %lu us", (unsigned long)requested);
            draw_menu(false);
            return;
        }
#endif
        const uint32_t actual = lgfx::epd2d_continuous_scan_get_line_period_us();
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int top = 108;
        constexpr int bottom_margin = 122;
        const int bottom = height - bottom_margin;
        const int split_y = top + (bottom - top) / 2;

        ESP_LOGI(TAG,
                 "timing A/B: begin line=%lu us nominal_phase=%lu us nominal_hz=%.1f",
                 (unsigned long)actual,
                 (unsigned long)(actual * MYPAPER_EPD_PANEL_HEIGHT),
                 actual ? 1000000.0f /
                              (float)(actual * MYPAPER_EPD_PANEL_HEIGHT) : 0.0f);

        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.fillRect(0, split_y, width, bottom - split_y, TFT_BLACK);
        finish_draw();

        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        char title[64] = {};
        snprintf(title, sizeof(title), ui_locale::text(ui_locale::TextId::QualityTimingFormat),
                 (unsigned long)actual);
        draw_header(title);
        for (int level = 0; level < 16; ++level) {
            const int x0 = level * width / 16;
            const int x1 = (level + 1) * width / 16;
            M5.Display.fillRect(x0, top, x1 - x0, bottom - top,
                                gray_color(level));
            char label[4] = {};
            snprintf(label, sizeof(label), "%d", level);
            system_font::draw_center(M5.Display, label, (x0 + x1) / 2,
                                     top + 12, level < 7 ? TFT_WHITE : TFT_BLACK);
        }
        M5.Display.fillRect(0, bottom, width, bottom_margin, TFT_WHITE);
        M5.Display.drawFastHLine(0, bottom, width, TFT_BLACK);
        draw_center_fit(ui_locale::text(actual == 20 ? ui_locale::TextId::Timing20Hint
                                                   : ui_locale::TextId::Timing16Hint),
                        bottom + 18);
        draw_center_fit(ui_locale::text(ui_locale::TextId::TopWhiteBottomBlack), bottom + 54);
        draw_center_fit(ui_locale::text(ui_locale::TextId::CompareGrayGhostScan), bottom + 86);
        finish_draw();
        ESP_LOGI(TAG, "timing A/B: complete line=%lu us", (unsigned long)actual);

    }

    void draw_quality_waveform_ab()
    {
#if MYPAPER_EPD_USE_2D_ENGINE
        auto* panel = static_cast<lgfx::Panel_EPD_2D*>(M5.Display.panel());
        const EpdWaveform* trimmed = my_waveform_gc16_36();
        if (panel == nullptr || trimmed == nullptr) {
            ESP_LOGE(TAG, "GC16 A/B: 36-phase table unavailable");
            draw_menu(false);
            return;
        }
        _view = View::QualityWaveformAB;
        const EpdWaveform* original = panel->waveform();
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int top = 96;
        constexpr int footer_h = 116;
        const int bottom = height - footer_h;
        const int black_bottom = top + (bottom - top) * 2 / 5;
        const int gray_bottom = top + (bottom - top) * 7 / 10;

        // Identical physical source for A and B. Always prepare it with the
        // normal 48-phase table; only the target refresh varies.
        panel->waveform(original);
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        finish_draw();
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        for (int x = 0; x < width; x += 48) {
            M5.Display.fillRect(x, top, std::min(24, width - x),
                                bottom - top, TFT_BLACK);
        }
        finish_draw();

        const int phases = _quality_36 ? 36 : 48;
        ESP_LOGI(TAG, "GC16 A/B: target begin phases=%d line=%lu us",
                 phases,
                 (unsigned long)lgfx::epd2d_continuous_scan_get_line_period_us());
        panel->waveform(_quality_36 ? trimmed : original);
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.fillRect(0, top, width, black_bottom - top, TFT_BLACK);
        for (int level = 0; level < 16; ++level) {
            const int x0 = level * width / 16;
            const int x1 = (level + 1) * width / 16;
            M5.Display.fillRect(x0, black_bottom, x1 - x0,
                                gray_bottom - black_bottom, gray_color(level));
            char label[4] = {};
            snprintf(label, sizeof(label), "%d", level);
            system_font::draw_center(M5.Display, label, (x0 + x1) / 2,
                                     black_bottom + 12,
                                     level < 7 ? TFT_WHITE : TFT_BLACK);
        }
        char title[48] = {};
        snprintf(title, sizeof(title), ui_locale::text(ui_locale::TextId::QualityGc16Format), phases);
        draw_header(title);
        draw_center_fit(ui_locale::text(ui_locale::TextId::InspectOldStripes),
                        bottom + 16);
        draw_center_fit(ui_locale::text(ui_locale::TextId::TapOtherWaveform), bottom + 58);
        finish_draw();
        panel->waveform(original);
        ESP_LOGI(TAG, "GC16 A/B: target complete phases=%d", phases);
#else
        draw_menu(false);
#endif
    }

    void draw_black_hold_white()
    {
        _view = View::BlackHoldWhite;
        _black_hold_refreshes = 0;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int footer_h = 116;
        const int bottom = height - footer_h;

        ESP_LOGI(TAG, "black hold: Quality white baseline begin");
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::QualityBlackHoldWhiteBaseline));
        M5.Display.drawFastHLine(0, bottom, width, TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::InspectWhiteTapBlack), bottom + 16);
        draw_center_fit(ui_locale::text(ui_locale::TextId::TapRepeatBlack), bottom + 58);
        finish_draw();
        ESP_LOGI(TAG, "black hold: Quality white baseline complete; waiting for tap");
    }

    void draw_black_hold_black()
    {
        const bool first_black = _view == View::BlackHoldWhite;
        _view = View::BlackHoldBlack;
        ++_black_hold_refreshes;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int top = 96;
        constexpr int footer_h = 116;
        const int bottom = height - footer_h;

        ESP_LOGI(TAG, "black hold: Quality %s begin refresh=%u",
                 first_black ? "white->black" : "black->black",
                 _black_hold_refreshes);
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.fillRect(0, top, width, bottom - top, TFT_BLACK);
        draw_header(ui_locale::text(ui_locale::TextId::QualityBlackHoldBlackTarget));
        M5.Display.drawFastHLine(0, bottom, width, TFT_BLACK);
        char caption[80] = {};
        snprintf(caption, sizeof(caption), ui_locale::text(ui_locale::TextId::BlackRefreshFormat),
                 _black_hold_refreshes,
                 ui_locale::text(first_black ? ui_locale::TextId::FromWhite : ui_locale::TextId::FromBlack));
        draw_center_fit(caption, bottom + 16);
        draw_center_fit(ui_locale::text(ui_locale::TextId::CompareDarknessRepeat), bottom + 58);
        finish_draw();
        ESP_LOGI(TAG, "black hold: Quality %s complete refresh=%u",
                 first_black ? "white->black" : "black->black",
                 _black_hold_refreshes);
    }

    void draw_direction_pattern()
    {
        _view = View::Direction;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);

        M5.Display.fillRect(0, 0, 150, 115, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TopLeft), 75, 32, TFT_WHITE);
        M5.Display.drawRect(width - 150, 0, 150, 115, TFT_BLACK);
        M5.Display.fillTriangle(width - 150, 0, width, 0, width, 115, TFT_BLACK);
        system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::TopRight), width - 16, 32, TFT_WHITE);

        M5.Display.drawRect(0, height - 115, 150, 115, TFT_BLACK);
        for (int y = height - 105; y < height - 10; y += 16) {
            M5.Display.fillRect(10, y, 130, 4, TFT_BLACK);
        }
        M5.Display.fillRect(width - 150, height - 115, 150, 115, TFT_BLACK);
        M5.Display.drawLine(width - 140, height - 105, width - 10, height - 10, TFT_WHITE);
        M5.Display.drawLine(width - 10, height - 105, width - 140, height - 10, TFT_WHITE);

        const int box_x = 24;
        const int box_w = width - 48;
        const int box_h = 96;
        const int first_y = 150;
        const int stripes[] = {1, 2, 4, 8, 16};
        for (int i = 0; i < 5; ++i) {
            const int y = first_y + i * 124;
            draw_vertical_stripes(box_x, y, box_w, box_h, stripes[i]);
            char label[24] = {};
            snprintf(label, sizeof(label), ui_locale::text(ui_locale::TextId::PixelVerticalFormat), stripes[i]);
            system_font::draw_left(M5.Display, label, box_x + 8, y + box_h + 5, TFT_BLACK);
        }

        const int horizontal_y = first_y + 5 * 124 + 20;
        const int cell_w = (box_w - 32) / 3;
        draw_horizontal_stripes(box_x, horizontal_y, cell_w, 150, 1);
        draw_horizontal_stripes(box_x + cell_w + 16, horizontal_y, cell_w, 150, 2);
        draw_horizontal_stripes(box_x + (cell_w + 16) * 2, horizontal_y, cell_w, 150, 4);

        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToReturn), width / 2,
                                 height - 165, TFT_BLACK);
        ESP_LOGI(TAG, "direction pattern: Quality refresh begin");
        finish_draw();
        ESP_LOGI(TAG, "direction pattern: complete");
    }

    void draw_grayscale_pattern(const char *caption)
    {
        _view = View::Grayscale;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int top = 92;
        const int bottom = height - 92;

        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        for (int level = 0; level < 16; ++level) {
            const int x0 = level * width / 16;
            const int x1 = (level + 1) * width / 16;
            M5.Display.fillRect(x0, top, x1 - x0, bottom - top,
                                gray_color(level));
        }
        system_font::draw_left(M5.Display, ui_locale::text(ui_locale::TextId::Gray0Black), 12, 18, TFT_BLACK);
        system_font::draw_right(M5.Display, ui_locale::text(ui_locale::TextId::Gray15White), width - 12, 18, TFT_BLACK);
        draw_center_fit(caption, height - 70);
        ESP_LOGI(TAG, "grayscale pattern: Quality refresh begin");
        finish_draw();
        ESP_LOGI(TAG, "grayscale pattern: complete");
    }

    void draw_grayscale_from(unsigned start_level, const char *caption)
    {
        _view = View::Grayscale;
        ESP_LOGI(TAG, "grayscale transition: establish level=%u Quality begin",
                 start_level);
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(gray_color(start_level));
        finish_draw();
        ESP_LOGI(TAG, "grayscale transition: base level=%u complete",
                 start_level);

        draw_grayscale_pattern(caption);
    }

    void draw_matrix_old()
    {
        _view = View::MatrixOld;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int footer_h = 104;

        begin_draw(epd_mode_t::epd_quality);
        for (int level = 0; level < 16; ++level) {
            const int x0 = level * width / 16;
            const int x1 = (level + 1) * width / 16;
            M5.Display.fillRect(x0, 0, x1 - x0, height,
                                gray_color(level));
        }
        M5.Display.fillRect(0, height - footer_h, width, footer_h, TFT_WHITE);
        M5.Display.drawFastHLine(0, height - footer_h, width, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::OldGrayColumns),
                                 width / 2, height - footer_h + 10, TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::TapNewGrayRows), height - footer_h + 54);
        ESP_LOGI(TAG, "transition matrix step 1: old columns Quality begin");
        finish_draw();
        ESP_LOGI(TAG, "transition matrix step 1 complete; waiting for confirmation");
    }

    void draw_matrix_target()
    {
        _view = View::MatrixTarget;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int footer_h = 104;
        const int pattern_h = height - footer_h;

        begin_draw(epd_mode_t::epd_quality);
        for (int level = 0; level < 16; ++level) {
            const int y0 = level * pattern_h / 16;
            const int y1 = (level + 1) * pattern_h / 16;
            M5.Display.fillRect(0, y0, width, y1 - y0,
                                gray_color(level));
        }
        M5.Display.fillRect(0, pattern_h, width, footer_h, TFT_WHITE);
        M5.Display.drawFastHLine(0, pattern_h, width, TFT_BLACK);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::NewGrayRows),
                                 width / 2, pattern_h + 10, TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::EachRowUniform), pattern_h + 54);
        ESP_LOGI(TAG, "transition matrix step 2: target rows Quality begin");
        finish_draw();
        ESP_LOGI(TAG, "transition matrix complete");
    }

    void draw_ghost_test_chrome()
    {
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        draw_header(ui_locale::text(ui_locale::TextId::QualityBlackToWhite));
        M5.Display.drawFastHLine(24, 112, width - 48, TFT_BLACK);
        M5.Display.drawFastHLine(24, height - 112, width - 48, TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::TapOkContinue), height - 82);
    }

    void draw_ghost_source()
    {
        _view = View::GhostSource;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int field_top = 140;
        const int field_bottom = height - 140;
        const int field_h = field_bottom - field_top;
        const int margin = 70;
        const int field_w = width - margin * 2;

        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        draw_ghost_test_chrome();

        // A mixture of solid masses, text-like bars and fine edges makes the
        // residual location and strength easy to judge after the white step.
        M5.Display.fillRect(margin, field_top, field_w / 3, field_h / 3,
                            TFT_BLACK);
        M5.Display.fillRect(width - margin - field_w / 3,
                            field_bottom - field_h / 3,
                            field_w / 3, field_h / 3, TFT_BLACK);
        M5.Display.drawRect(margin, field_top, field_w, field_h, TFT_BLACK);
        M5.Display.drawRect(margin + 12, field_top + 12,
                            field_w - 24, field_h - 24, TFT_BLACK);

        const int bar_x = margin + field_w / 3 + 45;
        const int bar_w = field_w / 3 - 90;
        for (int y = field_top + 70; y < field_bottom - 70; y += 78) {
            M5.Display.fillRect(bar_x, y, bar_w, 24, TFT_BLACK);
        }
        for (int x = margin + 35; x < width - margin - 35; x += 64) {
            M5.Display.drawFastVLine(x, field_top + field_h / 2 - 55,
                                     110, TFT_BLACK);
        }
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::BlackSourcePattern),
                                 width / 2, field_top + field_h / 2 + 88,
                                 TFT_BLACK);
        ESP_LOGI(TAG, "ghost test step 1: black source Quality begin");
        finish_draw();
        ESP_LOGI(TAG, "ghost test step 1 complete; waiting for white target");
    }

    void draw_ghost_target()
    {
        _view = View::GhostTarget;
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        draw_ghost_test_chrome();
        ESP_LOGI(TAG, "ghost test step 2: white target Quality begin");
        finish_draw();
        ESP_LOGI(TAG,
                 "ghost test complete: inspect central white field for black-source outline");
    }

    void run_vcom_measurement()
    {
        _view = View::Vcom;
        const int width = M5.Display.width();
        const int height = M5.Display.height();

        begin_draw(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::VcomMeasurement));
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::MeasuringKickback),
                                 width / 2, height / 2 - 40, TFT_BLACK);
        finish_draw();

        uint32_t measured_mV = 0;
        bool measured = false;
#if MYPAPER_EPD_USE_2D_ENGINE
        auto *panel = static_cast<lgfx::Panel_EPD_2D*>(M5.Display.panel());
        measured = panel != nullptr && panel->measureVCOM(&measured_mV, 800);
#endif
        ESP_LOGI(TAG, "VCOM: configured=%u mV measured_ok=%d kickback=%lu mV",
                 (unsigned)MYPAPER_EPD_VCOM_MV, measured,
                 (unsigned long)measured_mV);

        begin_draw(epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header(ui_locale::text(ui_locale::TextId::VcomMeasurement));
        char configured[64] = {};
        snprintf(configured, sizeof(configured), ui_locale::text(ui_locale::TextId::ConfiguredVoltageFormat),
                 (unsigned)(MYPAPER_EPD_VCOM_MV / 1000),
                 (unsigned)(MYPAPER_EPD_VCOM_MV % 1000));
        system_font::draw_center(M5.Display, configured, width / 2, 300, TFT_BLACK);

        char result[72] = {};
        if (measured) {
            snprintf(result, sizeof(result), ui_locale::text(ui_locale::TextId::KickbackVoltageFormat),
                     (unsigned long)(measured_mV / 1000),
                     (unsigned long)(measured_mV % 1000));
        } else {
            snprintf(result, sizeof(result), "%s", ui_locale::text(ui_locale::TextId::KickbackMeasurementFailed));
        }
        system_font::draw_center(M5.Display, result, width / 2, 390, TFT_BLACK);
        draw_center_fit(ui_locale::text(ui_locale::TextId::ComparePanelSpec), 500);
        draw_center_fit(ui_locale::text(ui_locale::TextId::MeasurementDoesNotChangeVcom), 555);
        system_font::draw_center(M5.Display, ui_locale::text(ui_locale::TextId::TapToReturn),
                                 width / 2, height - 110, TFT_BLACK);
        finish_draw();
    }
};

} // namespace

void launch_epd_diagnostics()
{
    ActivityManager::instance().push(new EpdDiagnosticActivity());
}
