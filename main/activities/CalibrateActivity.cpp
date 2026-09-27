#include "CalibrateActivity.h"
#include "ActivityManager.h"
#include "../touch_calibration.h"
#include "ui/SystemFont.h"
#include "ui/UiLocale.h"
#include <M5Unified.h>
#include <esp_log.h>
#include <cmath>

static const char *TAG = "CalAct";

// ─── Calibration target positions (screen-percentage based) ─────────────
// 4 corners at 15% / 85% of display area, computed at runtime

static struct { int sx, sy; } _targets[4];
static touch_cal::Point _collected[4];
static int _sw = 1280, _sh = 720;

static void _compute_targets(int sw, int sh) {
    _targets[0] = { sw * 15 / 100, sh * 15 / 100 };   // top-left
    _targets[1] = { sw * 85 / 100, sh * 15 / 100 };   // top-right
    _targets[2] = { sw * 85 / 100, sh * 85 / 100 };   // bottom-right
    _targets[3] = { sw * 15 / 100, sh * 85 / 100 };   // bottom-left
}

// ─── Drawing helpers ──────────────────────────────────────────────────

void CalibrateActivity::_draw_crosshair(int sx, int sy, int num) {
    auto &d = M5.Display;
    d.startWrite();
    d.fillScreen(TFT_WHITE);

    // Crosshair lines
    int len = 40;
    d.fillRect(sx - len, sy - 2, len * 2, 5, TFT_BLACK);
    d.fillRect(sx - 2, sy - len, 5, len * 2, TFT_BLACK);

    // Circle
    d.drawCircle(sx, sy, 18, TFT_BLACK);
    d.drawCircle(sx, sy, 20, TFT_BLACK);
    d.drawCircle(sx, sy, 22, TFT_DARKGRAY);

    // Point number
    d.setFont(&fonts::lv_font_montserrat_28);
    d.setTextColor(TFT_BLACK);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d", num, NUM_POINTS);
    d.drawString(buf, sx - 20, sy + 40);

    system_font::draw_center(d, ui_locale::text(ui_locale::TextId::TouchCrosshair),
                             _sw / 2, _sh - 40, TFT_BLACK);

    d.setEpdMode(epd_mode_t::epd_fastest);
    d.display();
    d.endWrite();
}

void CalibrateActivity::_draw_message(const char *line1, const char *line2) {
    auto &d = M5.Display;
    d.startWrite();
    d.fillScreen(TFT_WHITE);
    system_font::draw_center(d, line1, _sw / 2, _sh / 2 - 40, TFT_BLACK);
    if (line2) {
        system_font::draw_center(d, line2, _sw / 2, _sh / 2 + 20, TFT_BLACK);
    }
    d.setEpdMode(epd_mode_t::epd_fastest);
    d.display();
    d.endWrite();
}

void CalibrateActivity::_draw_success(float error_px) {
    auto &d = M5.Display;
    d.startWrite();
    d.fillScreen(TFT_WHITE);
    system_font::draw_center(d, ui_locale::text(ui_locale::TextId::CalibrationOk),
                             _sw / 2, _sh / 2 - 60, TFT_BLACK);
    char buf[64];
    snprintf(buf, sizeof(buf), ui_locale::text(ui_locale::TextId::CalibrationErrorFormat), error_px);
    system_font::draw_center(d, buf, _sw / 2, _sh / 2 - 10, TFT_BLACK);
    system_font::draw_center(d, ui_locale::text(ui_locale::TextId::StartingReader),
                             _sw / 2, _sh / 2 + 40, TFT_BLACK);
    d.setEpdMode(epd_mode_t::epd_fastest);
    d.display();
    d.endWrite();
}

void CalibrateActivity::_draw_fail() {
    auto &d = M5.Display;
    d.startWrite();
    d.fillScreen(TFT_WHITE);
    system_font::draw_center(d, ui_locale::text(ui_locale::TextId::CalibrationFailed),
                             _sw / 2, _sh / 2 - 60, TFT_BLACK);
    system_font::draw_center(d, ui_locale::text(ui_locale::TextId::TouchToRetry),
                             _sw / 2, _sh / 2 - 10, TFT_DARKGRAY);
    d.setEpdMode(epd_mode_t::epd_fastest);
    d.display();
    d.endWrite();
}

// ─── Activity lifecycle ───────────────────────────────────────────────

void CalibrateActivity::on_create() {
    _sw = M5.Display.width();
    _sh = M5.Display.height();
    _compute_targets(_sw, _sh);
    _state = DRAW;
    _point_idx = 0;
    _sample_cnt = 0;
    _acc_rx = _acc_ry = 0;
    _was_touched = false;

    ESP_LOGI(TAG, "Starting calibration (%dx%d)", _sw, _sh);
}

void CalibrateActivity::loop() {
    switch (_state) {

    // ── Draw current target ──────────────────────────────────────────
    case DRAW:
        if (_point_idx < NUM_POINTS) {
            _draw_crosshair(_targets[_point_idx].sx,
                           _targets[_point_idx].sy,
                           _point_idx + 1);
            _state = WAIT_TOUCH;
        } else {
            _state = DONE;
        }
        break;

    // ── Wait for touch down ──────────────────────────────────────────
    case WAIT_TOUCH: {
        lgfx::touch_point_t tp;
        int count = M5.Display.getTouchRaw(&tp, 1);
        bool touched = (count > 0);

        if (touched && !_was_touched) {
            // First touch → reset accumulator
            _acc_rx = tp.x;
            _acc_ry = tp.y;
            _sample_cnt = 1;
            _was_touched = true;
        } else if (touched && _sample_cnt < NUM_SAMPLES) {
            // Accumulate samples
            _acc_rx += tp.x;
            _acc_ry += tp.y;
            _sample_cnt++;
        } else if (!touched && _was_touched && _sample_cnt >= 4) {
            // Released → finalize this point
            float avg_rx = _acc_rx / _sample_cnt;
            float avg_ry = _acc_ry / _sample_cnt;
            _collected[_point_idx] = {
                (float)_targets[_point_idx].sx,
                (float)_targets[_point_idx].sy,
                avg_rx, avg_ry
            };
            ESP_LOGI(TAG, "Point %d: screen(%d,%d) raw(%.0f,%.0f) samples=%d",
                     _point_idx + 1,
                     _targets[_point_idx].sx, _targets[_point_idx].sy,
                     avg_rx, avg_ry, _sample_cnt);
            _was_touched = false;
            _sample_cnt = 0;
            _point_idx++;
            _state = DRAW;
        } else if (!touched) {
            _was_touched = false;
        }
        break;
    }

    // ── All points collected → compute & save ────────────────────────
    case DONE: {
        auto result = touch_cal::compute(_collected, NUM_POINTS);

        if (result.valid && result.error_px < 20.0f) {
            touch_cal::save(result.affine);
            M5.Display.panel()->setCalibrateAffine(result.affine);
            _draw_success(result.error_px);
            vTaskDelay(pdMS_TO_TICKS(2000));
            ActivityManager::instance().pop();  // proceed to main app
        } else {
            // High error or singular matrix → retry
            ESP_LOGW(TAG, "Calibration error=%.1fpx, retrying", result.error_px);
            _draw_fail();
            _was_touched = false;
            // Wait for touch to retry
            lgfx::touch_point_t tp;
            while (M5.Display.getTouchRaw(&tp, 1) == 0) {
                M5.update();
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            while (M5.Display.getTouchRaw(&tp, 1) > 0) {
                M5.update();
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            _point_idx = 0;
            _state = DRAW;
        }
        break;
    }
    }
}

// ─── Entry point ──────────────────────────────────────────────────────

bool CalibrateActivity::run_if_needed() {
    if (touch_cal::is_calibrated()) {
        // Apply saved calibration
        float affine[6];
        if (touch_cal::load(affine)) {
            M5.Display.panel()->setCalibrateAffine(affine);
        }
        return false;
    }
    // First boot — launch calibration
    ESP_LOGI(TAG, "No calibration found — launching calibration UI");
    ActivityManager::instance().push(new CalibrateActivity());
    return true;
}

void launch_calibration() {
    if (!touch_cal::is_calibrated()) {
        ActivityManager::instance().push(new CalibrateActivity());
    }
}
