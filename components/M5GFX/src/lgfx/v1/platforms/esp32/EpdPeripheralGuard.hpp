#pragma once
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace lgfx { inline namespace v1 {
// One process-wide gate shared by the S31 scan task and camera sessions. Take
// before draw/statistics locks; never wait for display completion while held.
inline SemaphoreHandle_t epdPeripheralMutex() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
    return mutex;
}
class EpdPeripheralGuard {
public:
    explicit EpdPeripheralGuard(uint32_t timeout_ms = UINT32_MAX)
        : _locked(xSemaphoreTake(epdPeripheralMutex(), timeout_ms == UINT32_MAX
                  ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {}
    ~EpdPeripheralGuard() { if (_locked) xSemaphoreGive(epdPeripheralMutex()); }
    explicit operator bool() const { return _locked; }
    EpdPeripheralGuard(const EpdPeripheralGuard&) = delete;
    EpdPeripheralGuard& operator=(const EpdPeripheralGuard&) = delete;
private:
    bool _locked;
};
} }
