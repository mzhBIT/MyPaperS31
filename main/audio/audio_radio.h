#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_PLAYBACK_IDLE = 0,
    AUDIO_PLAYBACK_STOPPED,
    AUDIO_PLAYBACK_PLAYING,
    AUDIO_PLAYBACK_PAUSED,
    AUDIO_PLAYBACK_FINISHED,
    AUDIO_PLAYBACK_ERROR,
} audio_playback_state_t;

esp_err_t audio_radio_init(void);
esp_err_t audio_radio_play(const char *url);
esp_err_t audio_radio_play_file(const char *path);
esp_err_t audio_radio_play_default(void);
esp_err_t audio_radio_stop(void);
esp_err_t audio_radio_pause(void);
esp_err_t audio_radio_resume(void);
audio_playback_state_t audio_radio_get_state(void);
bool audio_radio_is_playback_ready(void);
esp_err_t audio_radio_get_file_progress(uint32_t *progress_permille);
esp_err_t audio_radio_get_file_timing(uint32_t *elapsed_ms, uint32_t *duration_ms);
esp_err_t audio_radio_set_volume(int volume);
int audio_radio_get_volume(void);
esp_err_t audio_radio_record_wav(const char *path, uint32_t duration_ms);
void audio_radio_deinit(void);
void *audio_radio_get_player(void);

#ifdef __cplusplus
}
#endif
