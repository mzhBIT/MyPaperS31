// Explicit boundaries until S31 BH1098G / USB / ML307 drivers are adapted.
// Do not silently run the P4 ES8311, GPIO54 C6 or USB-switch wiring here.
#include "audio/audio_radio.h"
#include "modem/ml307_sms.h"
#include "modem/ml307_voice.h"
#include "usb/usb_modem_probe.h"
#include "usb/usb_msc_mode.h"
#include "usb/usb_transfer_boot.h"
#include "frontlight/frontlight.h"
#include <esp_log.h>

namespace {
esp_err_t unavailable(const char* feature) {
    ESP_LOGW("S31_HW", "%s backend not adapted on MyPaperS31", feature);
    return ESP_ERR_NOT_SUPPORTED;
}
int volume = 30;
audio_output_mode_t output = AUDIO_OUTPUT_AUTO;
}
extern "C" {
esp_err_t audio_radio_init() { return unavailable("BH1098G audio"); }
esp_err_t audio_radio_play(const char*) { return audio_radio_init(); }
esp_err_t audio_radio_play_file(const char*) { return audio_radio_init(); }
void audio_radio_force_speaker(bool) {}
esp_err_t audio_radio_play_default() { return audio_radio_init(); }
esp_err_t audio_radio_stop() { return ESP_OK; }
esp_err_t audio_radio_pause() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_radio_resume() { return ESP_ERR_NOT_SUPPORTED; }
audio_playback_state_t audio_radio_get_state() { return AUDIO_PLAYBACK_IDLE; }
bool audio_radio_is_playback_ready() { return false; }
esp_err_t audio_radio_get_file_progress(uint32_t* value) { if (value) *value=0; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_radio_get_file_timing(uint32_t* elapsed, uint32_t* duration) {
    if (elapsed) *elapsed=0;
    if (duration) *duration=0;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t audio_radio_set_volume(int value) { volume=value; return ESP_ERR_NOT_SUPPORTED; }
int audio_radio_get_volume() { return volume; }
esp_err_t audio_radio_set_output_mode(audio_output_mode_t mode) { output=mode; return ESP_ERR_NOT_SUPPORTED; }
audio_output_mode_t audio_radio_get_output_mode() { return output; }
const char* audio_radio_get_output_mode_name() { return "Unavailable"; }
const char* audio_radio_get_active_output_name() { return "Unavailable"; }
bool audio_radio_usb_connected() { return false; }
void audio_radio_service_output() {}
esp_err_t audio_radio_record_wav(const char*, uint32_t) { return unavailable("audio input"); }
void audio_radio_deinit() {}
void* audio_radio_get_player() { return nullptr; }
esp_err_t usb_modem_probe_start() { return unavailable("ML307 USB"); }
bool usb_modem_probe_is_online() { return false; }
esp_err_t usb_modem_probe_wait_online(int) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t usb_msc_mode_run() { return unavailable("USB MSC"); }
bool usb_transfer_boot_requested() { return false; }
esp_err_t usb_transfer_boot_set(bool) { return unavailable("USB MSC"); }
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
