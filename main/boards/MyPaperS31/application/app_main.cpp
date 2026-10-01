#include "boards/Board.h"
#include "boards/MyPaperS31/Display.h"
#include <nvs_flash.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Stage 2: validate display/gray transitions/touch before enabling the reader.
static unsigned page = 0;
static void draw() {
    auto& gfx = mypaper::board::s31::display();
    gfx.waitDisplay();
    gfx.setEpdMode(lgfx::epd_mode_t::epd_quality);
    gfx.startWrite();
    gfx.fillScreen(TFT_WHITE);
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextSize(2);
    gfx.setCursor(20, 20);
    gfx.printf("MyPaperS31  Display / Touch test %u\n", page + 1);
    gfx.drawFastHLine(20, 72, gfx.width() - 40, TFT_BLACK);
    if (page == 0) {
        gfx.setCursor(20, 100);
        gfx.println("Tap: next pattern");
        gfx.println("Stage 2 - reader not enabled yet");
        for (int i = 0; i < 16; ++i) {
            const int x0 = 20 + (gfx.width() - 40) * i / 16;
            const int x1 = 20 + (gfx.width() - 40) * (i + 1) / 16;
            const uint8_t gray = i * 17;
            gfx.fillRect(x0, 240, x1 - x0, 300, gfx.color888(gray, gray, gray));
        }
    } else {
        // Both source extremes are kept on screen for the following pattern.
        const int top = 100;
        const int half = (gfx.height() - top - 40) / 2;
        if (page == 1) {
            gfx.fillRect(20, top, gfx.width() - 40, half, TFT_BLACK);
        } else if (page == 2) {
            for (int i = 0; i < 16; ++i) {
                const int x0 = 20 + (gfx.width() - 40) * i / 16;
                const int x1 = 20 + (gfx.width() - 40) * (i + 1) / 16;
                const uint8_t gray = i * 17;
                gfx.fillRect(x0, top, x1 - x0, half * 2, gfx.color888(gray, gray, gray));
            }
        } else {
            gfx.fillRect(20, top + half, gfx.width() - 40, half, TFT_BLACK);
        }
    }
    gfx.display();
    gfx.endWrite();
    ESP_LOGI("S31_UI", "pattern=%u queued", page);
}
extern "C" void app_main() {
    // Do not erase NVS automatically: retain existing user data on errors.
    const esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK) ESP_LOGW("S31_UI", "NVS unavailable: %s", esp_err_to_name(nvs));
    mypaper::board::log_profile();
    if (!mypaper::board::initialize_hardware(false).display_ready) {
        ESP_LOGE("S31_UI", "display init failed; stopped (no reboot loop)");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    auto& gfx = mypaper::board::s31::display();
    gfx.setRotation(0);
    draw();
    bool was_pressed = false;
    for (;;) {
        // Never redraw or touch I2C while the waveform owns the panel bus.
        if (!gfx.displayBusy()) {
            uint16_t x = 0, y = 0;
            const bool pressed = gfx.getTouch(&x, &y);
            if (pressed && !was_pressed) {
                ESP_LOGI("S31_UI", "touch x=%u y=%u", x, y);
                page = (page + 1) % 4;
                draw();
            }
            was_pressed = pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
