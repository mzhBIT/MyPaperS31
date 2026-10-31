#include "UsbAudioPort.h"
#include "UsbSwitch.h"
#include "Pins.h"
#include <M5Unified.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>

namespace {
constexpr const char *TAG = "S31_USB_AUDIO";
constexpr uint8_t registers[] = {0x0a,0x0b,0x0c,0x0d,0x16,0x18};
uint8_t saved[sizeof(registers)]{};
bool owned = false, ready = false;
uint32_t last_blocked_state=0xffffffff;
std::mutex lock;
bool read(uint8_t reg, uint8_t &value) {
    return m5::In_I2C.readRegister(mypaper::board::s31::charger_address, reg, &value, 1, 100000);
}
bool write(uint8_t reg, uint8_t value) {
    return m5::In_I2C.writeRegister8(mypaper::board::s31::charger_address, reg, value, 100000);
}
bool restore() {
    ready=false;
    // Disable the source first, then restore only the OTG/charge fields we own.
    // Never restore status bits or change unrelated charger limits.
    uint8_t current = 0;
    bool ok = read(0x18, current) && write(0x18, current & ~0x40);
    if (!ok) { ESP_LOGE(TAG,"Cannot disable charger OTG"); return false; }
    const uint8_t masks[] = {0xf0,0x0f,0xc0,0x1f,0x33};
    for (unsigned i=0;i<sizeof(masks);++i) {
        bool step = read(registers[i],current) &&
            write(registers[i], (current & ~masks[i]) | (saved[i] & masks[i]));
        ok = step && ok;
    }
    if (!ok) ESP_LOGE(TAG,"Charger OTG restore failed");
    mypaper::board::s31::usb_analog_switch_set(false);
    owned = !ok;
    return ok;
}
}

extern "C" esp_err_t s31_usb_audio_port_acquire() {
    std::lock_guard<std::mutex> guard(lock);
    if (owned && ready) return ESP_OK;
    if (owned && !restore()) return ESP_FAIL;
    uint8_t status = 0, control1 = 0, control3 = 0;
    if (!read(0x1e,status)) return ESP_ERR_NOT_FOUND;
    if (!read(0x16,control1) || !read(0x18,control3)) return ESP_FAIL;
    // Charger registers survive an MCU-only reset. In normal application mode
    // this module is the board's only OTG owner (MSC is a separate device boot).
    // A confirmed OTG source can therefore be reclaimed after a reset. Keep
    // blocking all external/ambiguous states; never force boost into an input.
    const bool retained_otg=(status & 7)==7 && (control3 & 0x40);
    if (!retained_otg && ((status & 7) != 0 || (control3 & 0x40))) {
        const uint32_t state=(uint32_t(status)<<16) | (uint32_t(control1)<<8) | control3;
        if (state!=last_blocked_state) {
            ESP_LOGW(TAG,"Host power blocked: STATUS1=0x%02x VBUS_type=%u CTRL1=0x%02x CTRL3=0x%02x OTG_enabled=%u",
                     status,unsigned(status&7),control1,control3,unsigned((control3>>6)&1));
            last_blocked_state=state;
        }
        return ESP_ERR_INVALID_STATE;
    }
    last_blocked_state=0xffffffff;
    for (unsigned i=0;i<sizeof(registers);++i)
        if (!read(registers[i],saved[i])) return ESP_FAIL;
    if (bool(saved[5] & 0x40)!=retained_otg) {
        ESP_LOGW(TAG,"Host power state changed during acquisition: CTRL3=0x%02x",saved[5]);
        return ESP_ERR_INVALID_STATE;
    }
    if (retained_otg) {
        // The pre-reset owner already disabled charging. Restore the reference
        // board's normal charge-enabled state when this new session releases.
        saved[4] |= 0x20;
        ESP_LOGI(TAG,"Reclaiming retained OTG: STATUS1=0x%02x CTRL1=0x%02x CTRL3=0x%02x",
                 status,control1,control3);
    }
    owned = true; // All rollback data is valid before the first register write.
    // Reference CX25601N sequence: charge/HIZ/watchdog off, 5040 mV, 1000 mA,
    // then EN_OTG. Preserve all other charger fields.
    const uint8_t values[] = {
        uint8_t((saved[0]&0x0f) | ((1000/20 & 15)<<4)),
        uint8_t((saved[1]&0xf0) | ((1000/20>>4)&15)),
        uint8_t((saved[2]&0x3f) | ((5040/80 & 3)<<6)),
        uint8_t((saved[3]&0xe0) | ((5040/80>>2)&31)),
        uint8_t(saved[4] & ~0x33), uint8_t(saved[5] | 0x40)};
    // Disable charging before programming/enabling boost.
    bool ok = write(0x16, values[4]);
    for (unsigned i=0;i<4 && ok;++i) ok = write(registers[i],values[i]);
    if (ok) ok = write(0x18,values[5]);
    if (!ok) { restore(); return ESP_FAIL; }
    vTaskDelay(pdMS_TO_TICKS(30));
    if (!read(0x18,status) || !(status & 0x40)) { restore(); return ESP_FAIL; }
    mypaper::board::s31::usb_analog_switch_set(true);
    ready=true;
    ESP_LOGI(TAG,"Type-C HS Host route selected, VBUS OTG 5040 mV / 1000 mA");
    return ESP_OK;
}

extern "C" void s31_usb_audio_port_release() {
    std::lock_guard<std::mutex> guard(lock);
    for (int attempt=0;owned && attempt<3;++attempt) {
        if (restore()) ESP_LOGI(TAG,"VBUS OTG off; Serial/JTAG route restored");
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" bool s31_usb_audio_port_is_released() {
    std::lock_guard<std::mutex> guard(lock);
    return !owned;
}
