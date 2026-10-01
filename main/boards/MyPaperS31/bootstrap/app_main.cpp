#include "boards/Board.h"
#include "boards/MyPaperS31/Pins.h"
#include "boards/MyPaperS31/Pins.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
extern "C" void app_main() {
    static_assert(mypaper::board::s31::epd_data.size() == 8);
    static_assert(mypaper::board::s31::epd_data.size() == 8);
    mypaper::board::log_profile();
    ESP_LOGW("BOARD", "S31 bootstrap only: display/touch/SD/audio not initialized; no app UI yet");
    // No NVS writes or unverified power/GPIO operations in stage 1.
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
