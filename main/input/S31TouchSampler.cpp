#include "S31TouchSampler.h"
#include <sdkconfig.h>
#if CONFIG_IDF_TARGET_ESP32S31
#include "boards/MyPaperS31/Pins.h"
#include <M5Unified.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>

namespace s31_touch {
namespace {
constexpr auto irq = static_cast<gpio_num_t>(mypaper::board::s31::touch_interrupt);
constexpr const char* TAG = "S31_TOUCH";
QueueHandle_t samples = nullptr;
SemaphoreHandle_t ready = nullptr;
TaskHandle_t worker = nullptr;
std::atomic<uint32_t> epoch{0};
std::atomic<bool> overflow{false}, paused{false}, pause_ack{false}, down{false};
bool irq_installed = false;

void IRAM_ATTR interrupt(void* argument) {
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(argument), &wake);
    if (wake) portYIELD_FROM_ISR();
}
void publish(const Sample& sample) {
    if (xQueueSend(samples, &sample, 0) != pdTRUE) overflow.store(true);
    xSemaphoreGive(ready);
}
void scan(void*) {
    Sample previous{};
    bool have_previous = false, read_failed = false;
    for (;;) {
        if (paused.load()) {
            pause_ack.store(true); // No I2C transaction is in progress now.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            have_previous = false;
            continue;
        }
        pause_ack.store(false);
        Sample sample{};
        sample.epoch = epoch.load(); // Capture before I2C, discard raced samples.
        auto* touch = M5.Display.touch();
        lgfx::touch_point_t point{};
        const int count = touch->getTouchRaw(&point, 1); // No drawing/panel locks.
        sample.at = lgfx::millis();
        sample.valid = touch->lastReadSucceeded();
        sample.down = count > 0;
        sample.x = point.x;
        sample.y = point.y;
        if (!sample.valid) {
            // A bus error is not a release. Cancel this contact on the UI thread.
            if (!read_failed) {
                ESP_LOGW(TAG, "touch I2C read failed; cancelling contact");
                publish(sample);
            }
            read_failed = true;
        } else {
            down.store(sample.down);
            if (read_failed) ESP_LOGI(TAG, "touch I2C recovered");
            const bool changed = !have_previous || read_failed || sample.epoch != previous.epoch
                || sample.down != previous.down
                || (sample.down && (sample.x != previous.x || sample.y != previous.y));
            // Keep every down/up and movement; stationary holds need only a heartbeat.
            if (changed || (sample.down && uint32_t(sample.at-previous.at) >= 20)) {
                publish(sample);
                previous = sample;
                have_previous = true;
            }
            read_failed = false;
        }
        // GPIO5 wakes idle sampling immediately. Poll while held to detect motion
        // and release even if INT stays low; idle fallback tolerates missing edges.
        if (down.load() || read_failed) {
            ulTaskNotifyTake(pdTRUE, 0);
            vTaskDelay(1); // Repeated INT edges must not create a busy read loop.
        } else {
            ulTaskNotifyTake(pdTRUE, std::max<TickType_t>(1, pdMS_TO_TICKS(50)));
        }
    }
}
}
bool init() {
    if (worker) return true;
    if (!M5.Display.touch()) return false;
    samples = xQueueCreate(128, sizeof(Sample));
    ready = xSemaphoreCreateBinary();
    if (!samples || !ready ||
        xTaskCreatePinnedToCore(scan, "s31_touch", 4096, nullptr, 3, &worker, 0) != pdPASS) {
        if (samples) vQueueDelete(samples);
        if (ready) vSemaphoreDelete(ready);
        samples = nullptr;
        ready = nullptr;
        worker = nullptr;
        ESP_LOGW(TAG, "sampler allocation failed; main-thread polling retained");
        return false;
    }
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << irq;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.intr_type = GPIO_INTR_ANYEDGE;
    auto result = gpio_config(&config);
    if (result == ESP_OK) {
        result = gpio_install_isr_service(0); // Shared with NT26, never uninstall it.
        if (result == ESP_OK || result == ESP_ERR_INVALID_STATE)
            result = gpio_isr_handler_add(irq, interrupt, worker);
    }
    irq_installed = result == ESP_OK;
    if (!irq_installed) ESP_LOGW(TAG, "GPIO5 IRQ unavailable (%s); independent polling retained", esp_err_to_name(result));
    ESP_LOGI(TAG, "GPIO5 IRQ=%d, independent sampler, active=1 tick idle fallback=50ms", irq_installed);
    return true;
}
bool running() { return worker != nullptr; }
bool pop(Sample* sample) {
    if (!samples) return false;
    while (xQueueReceive(samples, sample, 0) == pdTRUE)
        if (sample->epoch == epoch.load()) return true;
    return false;
}
bool pending() { return samples && uxQueueMessagesWaiting(samples); }
bool take_overflow() { return overflow.exchange(false); }
void discard() {
    if (samples) xQueueReset(samples);
    overflow.store(false);
    // Increment after resetting: a new-epoch idle sample must not be erased,
    // otherwise cancellation could swallow the next fresh tap waiting for UP.
    ++epoch;
    if (worker) xTaskNotifyGive(worker);
}
void wait(bool fast) {
    const TickType_t ticks = fast ? 1 : std::max<TickType_t>(1, pdMS_TO_TICKS(20));
    if (ready) xSemaphoreTake(ready, ticks);
    else vTaskDelay(ticks);
}
bool pause() {
    if (!worker) return true;
    paused.store(true);
    if (irq_installed) gpio_intr_disable(irq);
    xTaskNotifyGive(worker);
    const auto start = lgfx::millis();
    while (!pause_ack.load() && uint32_t(lgfx::millis()-start) < 1000) vTaskDelay(1);
    if (pause_ack.load()) return true;
    resume();
    return false;
}
void resume() {
    discard();
    pause_ack.store(false);
    paused.store(false);
    if (irq_installed) gpio_intr_enable(irq);
    if (worker) xTaskNotifyGive(worker);
}
}
#endif
