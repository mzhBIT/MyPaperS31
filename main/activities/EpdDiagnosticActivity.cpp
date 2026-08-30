#include "activities/EpdDiagnosticActivity.h"

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "ui/SystemFont.h"

#include <M5Unified.h>
#include <esp_log.h>

#include "lgfx/v1/platforms/esp32/MyPaper_EPD_Config.hpp"
#include "lgfx/v1/platforms/esp32/Panel_EPD_2D.hpp"

#include <algorithm>
#include <cstdio>

namespace {

constexpr int HEADER_H = 76;
constexpr int PAD = 20;
constexpr int MENU_TOP = 108;
constexpr int MENU_ROW_H = 94;
constexpr int MENU_ROW_GAP = 8;
constexpr int MENU_COUNT = 9;

const char *TAG = "EPD_DIAG";

const char *MENU_LABELS[MENU_COUNT] = {
    "Direction & pixel order",
    "16-level grayscale",
    "White -> 16 gray",
    "Black -> 16 gray",
    "16 x 16 transitions",
    "Quality black -> white",
    "Text / DU4 four-gray",
    "Fastest / A2 binary",
    "VCOM measurement",
};

void draw_header(const char *title)
{
    M5.Display.fillRect(0, 0, M5.Display.width(), HEADER_H, TFT_WHITE);
    system_font::draw_center(M5.Display, title, M5.Display.width() / 2,
                             std::max(0, (HEADER_H - system_font::line_height()) / 2),
                             TFT_BLACK);
    M5.Display.drawFastHLine(0, HEADER_H - 1, M5.Display.width(), TFT_BLACK);
}

bool header_back_hit(const TouchEvent &event)
{
    return event.type == TouchEventType::Tap && event.x < 120 && event.y < HEADER_H;
}

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

class EpdDiagnosticActivity final : public Activity {
public:
    void on_create() override
    {
        draw_menu(true);
    }

    bool on_touch(const TouchEvent &event) override
    {
        if (event.type == TouchEventType::Back || header_back_hit(event)) {
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
        Text4Gray,
        A2Binary,
        Vcom,
    };

    View _view = View::Menu;
    int _focus = 0;

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
        _view = View::Menu;
        begin_draw(quality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
        M5.Display.fillScreen(TFT_WHITE);
        draw_header("EPD Diagnostics");

        const int width = M5.Display.width();
        for (int i = 0; i < MENU_COUNT; ++i) {
            const int top = MENU_TOP + i * (MENU_ROW_H + MENU_ROW_GAP);
            const bool selected = i == _focus;
            const uint16_t background = selected ? TFT_BLACK : TFT_WHITE;
            const uint16_t foreground = selected ? TFT_WHITE : TFT_BLACK;
            M5.Display.fillRoundRect(PAD, top, width - PAD * 2,
                                     MENU_ROW_H, 8, background);
            M5.Display.drawRoundRect(PAD, top, width - PAD * 2,
                                     MENU_ROW_H, 8, TFT_BLACK);
            system_font::draw_left(M5.Display, MENU_LABELS[i], PAD + 18,
                                   top + std::max(0, (MENU_ROW_H - system_font::line_height()) / 2),
                                   foreground);
            system_font::draw_right(M5.Display, ">", width - PAD - 20,
                                    top + std::max(0, (MENU_ROW_H - system_font::line_height()) / 2),
                                    foreground);
        }

        draw_center_fit("Quality / GC16, Text / DU4 and Fastest / A2",
                        M5.Display.height() - 100);
        finish_draw();
    }

    void run_selected()
    {
        switch (_focus) {
            case 0: draw_direction_pattern(); break;
            case 1: draw_grayscale_pattern("Direct 16-level / tap to return"); break;
            case 2: draw_grayscale_from(15, "WHITE -> 16 gray / tap to return"); break;
            case 3: draw_grayscale_from(0, "BLACK -> 16 gray / tap to return"); break;
            case 4: draw_matrix_old(); break;
            case 5: draw_ghost_source(); break;
            case 6: draw_text_four_gray(); break;
            case 7: draw_a2_binary(); break;
            case 8: run_vcom_measurement(); break;
            default: break;
        }
    }

    void draw_text_four_gray()
    {
        _view = View::Text4Gray;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        constexpr int header_h = 96;
        constexpr int footer_h = 116;
        const int pattern_top = header_h;
        const int pattern_bottom = height - footer_h;
        const int pattern_h = pattern_bottom - pattern_top;
        const int split_y = pattern_top + pattern_h / 2;

        // Establish two deliberately different physical starting states with
        // GC16.  The following DU4 result must converge to identical shades
        // above and below the center line.
        ESP_LOGI(TAG, "text four-gray: establish white/black split Quality begin");
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.fillRect(0, split_y, width, height - split_y,
                            TFT_BLACK);
        finish_draw();
        ESP_LOGI(TAG, "text four-gray: base split complete");

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
        system_font::draw_left(M5.Display, "0 BLACK", 12, 18, TFT_BLACK);
        system_font::draw_right(M5.Display, "15 WHITE", width - 12, 18,
                                TFT_BLACK);
        draw_center_fit("Text / DU4: four columns should be uniform", pattern_bottom + 16);
        draw_center_fit("No seam between white-start and black-start halves", pattern_bottom + 58);
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
        system_font::draw_left(M5.Display, "BLACK", 12, 18, TFT_BLACK);
        system_font::draw_right(M5.Display, "WHITE", width - 12, 18,
                                TFT_BLACK);
        draw_center_fit("Fastest / A2: alternating black and white columns",
                        pattern_bottom + 16);
        draw_center_fit("No seam between white-start and black-start halves",
                        pattern_bottom + 58);
        ESP_LOGI(TAG, "A2 binary: epd_fastest refresh begin");
        finish_draw();
        ESP_LOGI(TAG, "A2 binary: complete");
    }

    void draw_direction_pattern()
    {
        _view = View::Direction;
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        begin_draw(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);

        M5.Display.fillRect(0, 0, 150, 115, TFT_BLACK);
        system_font::draw_center(M5.Display, "TL", 75, 32, TFT_WHITE);
        M5.Display.drawRect(width - 150, 0, 150, 115, TFT_BLACK);
        M5.Display.fillTriangle(width - 150, 0, width, 0, width, 115, TFT_BLACK);
        system_font::draw_right(M5.Display, "TR", width - 16, 32, TFT_WHITE);

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
            snprintf(label, sizeof(label), "%d px vertical", stripes[i]);
            system_font::draw_left(M5.Display, label, box_x + 8, y + box_h + 5, TFT_BLACK);
        }

        const int horizontal_y = first_y + 5 * 124 + 20;
        const int cell_w = (box_w - 32) / 3;
        draw_horizontal_stripes(box_x, horizontal_y, cell_w, 150, 1);
        draw_horizontal_stripes(box_x + cell_w + 16, horizontal_y, cell_w, 150, 2);
        draw_horizontal_stripes(box_x + (cell_w + 16) * 2, horizontal_y, cell_w, 150, 4);

        system_font::draw_center(M5.Display, "Tap to return", width / 2,
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
        system_font::draw_left(M5.Display, "0 BLACK", 12, 18, TFT_BLACK);
        system_font::draw_right(M5.Display, "15 WHITE", width - 12, 18, TFT_BLACK);
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
        system_font::draw_center(M5.Display, "OLD gray = columns 0..15",
                                 width / 2, height - footer_h + 10, TFT_BLACK);
        draw_center_fit("Tap OK for NEW gray rows", height - footer_h + 54);
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
        system_font::draw_center(M5.Display, "NEW gray = rows 0..15",
                                 width / 2, pattern_h + 10, TFT_BLACK);
        draw_center_fit("Each row should be uniform / tap to return", pattern_h + 54);
        ESP_LOGI(TAG, "transition matrix step 2: target rows Quality begin");
        finish_draw();
        ESP_LOGI(TAG, "transition matrix complete");
    }

    void draw_ghost_test_chrome()
    {
        const int width = M5.Display.width();
        const int height = M5.Display.height();
        draw_header("Quality Black -> White");
        M5.Display.drawFastHLine(24, 112, width - 48, TFT_BLACK);
        M5.Display.drawFastHLine(24, height - 112, width - 48, TFT_BLACK);
        draw_center_fit("Tap / OK to continue", height - 82);
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
        system_font::draw_center(M5.Display, "BLACK SOURCE PATTERN",
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
        draw_header("VCOM Measurement");
        system_font::draw_center(M5.Display, "Measuring TPS65185 kick-back...",
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
        draw_header("VCOM Measurement");
        char configured[64] = {};
        snprintf(configured, sizeof(configured), "Configured: -%u.%03u V",
                 (unsigned)(MYPAPER_EPD_VCOM_MV / 1000),
                 (unsigned)(MYPAPER_EPD_VCOM_MV % 1000));
        system_font::draw_center(M5.Display, configured, width / 2, 300, TFT_BLACK);

        char result[72] = {};
        if (measured) {
            snprintf(result, sizeof(result), "Kick-back: -%lu.%03lu V",
                     (unsigned long)(measured_mV / 1000),
                     (unsigned long)(measured_mV % 1000));
        } else {
            snprintf(result, sizeof(result), "Kick-back measurement failed");
        }
        system_font::draw_center(M5.Display, result, width / 2, 390, TFT_BLACK);
        draw_center_fit("Compare with panel label / official specification", 500);
        draw_center_fit("Measurement does not change VCOM", 555);
        system_font::draw_center(M5.Display, "Tap to return",
                                 width / 2, height - 110, TFT_BLACK);
        finish_draw();
    }
};

} // namespace

void launch_epd_diagnostics()
{
    ActivityManager::instance().push(new EpdDiagnosticActivity());
}
