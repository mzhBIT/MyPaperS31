// P4 ML307 USB/SMS/voice API boundaries. S31 NT26 data lives in cellular/.
// Do not silently run the P4 ES8311, GPIO54 C6 or USB-switch wiring here.
// Speaker playback lives in audio/audio_radio_s31.cpp.
#include "modem/ml307_sms.h"
#include "modem/ml307_voice.h"
#include "usb/usb_modem_probe.h"
#include "frontlight/frontlight.h"
#include <esp_log.h>

namespace {
esp_err_t unavailable(const char* feature) {
    ESP_LOGW("S31_HW", "%s backend not adapted on MyPaperS31", feature);
    return ESP_ERR_NOT_SUPPORTED;
}
}
extern "C" {
esp_err_t usb_modem_probe_start() { return unavailable("ML307 USB"); }
bool usb_modem_probe_is_online() { return false; }
esp_err_t usb_modem_probe_wait_online(int) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ml307_sms_transport_init() { return unavailable("ML307 AT"); }
esp_err_t ml307_sms_transport_enable(uint8_t) { return ESP_ERR_NOT_SUPPORTED; }
bool ml307_sms_is_ready() { return false; }
bool ml307_sms_take_new_message_hint() { return false; }
esp_err_t ml307_sms_list(ml307_sms_message_t*, size_t, size_t* count) {
    if (count) *count=0;
    return unavailable("SMS");
}
esp_err_t ml307_sms_read(int, ml307_sms_message_t*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ml307_sms_send(const char*, const char*) { return unavailable("SMS"); }
esp_err_t ml307_sms_get_time_utc(int64_t*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ml307_sms_get_time(char*, size_t, int64_t*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ml307_sms_delete(int) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ml307_voice_query(ml307_call_info_t* info) {
    if (info) *info={};
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ml307_voice_dial(const char*) { return unavailable("phone"); }
esp_err_t ml307_voice_answer() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ml307_voice_hangup() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ml307_voice_send_dtmf(char) { return ESP_ERR_NOT_SUPPORTED; }
bool ml307_voice_take_ring_hint() { return false; }
}
