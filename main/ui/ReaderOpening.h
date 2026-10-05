#pragma once

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>

// Overlap white scanout with data-only reader construction. Call finish()
// BEFORE ActivityManager::push(), whose on_create() starts drawing the page.
// No panel mode changes or framebuffer writes are permitted between these.
class ReaderOpeningWhite {
public:
    ReaderOpeningWhite()
    {
        M5.Display.waitDisplay();
        _previous_mode = M5.Display.getEpdMode();
        M5.Display.getPanel()->setPageTurnAnimation(0);
        M5.Display.startWrite();
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.display();
        M5.Display.endWrite();
        _load_begin_us = esp_timer_get_time();
    }
    ReaderOpeningWhite(const ReaderOpeningWhite &) = delete;
    ReaderOpeningWhite &operator=(const ReaderOpeningWhite &) = delete;
    ~ReaderOpeningWhite() { finish(); }

    void finish()
    {
        if (_finished) return;
        const int64_t wait_begin = esp_timer_get_time();
        M5.Display.waitDisplay();
        const int64_t wait_us = esp_timer_get_time() - wait_begin;
        M5.Display.setEpdMode(_previous_mode);
        _finished = true;
        // Print after scanout, not in the time-critical white refresh path.
        ESP_LOGI("READER_OPEN", "data preparation=%lld ms white wait=%lld ms",
                 (wait_begin - _load_begin_us) / 1000, wait_us / 1000);
    }
private:
    epd_mode_t _previous_mode;
    int64_t _load_begin_us = 0;
    bool _finished = false;
};
