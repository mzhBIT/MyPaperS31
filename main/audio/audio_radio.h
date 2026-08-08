#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_radio_init(void);
esp_err_t audio_radio_play(const char *url);
esp_err_t audio_radio_play_default(void);
esp_err_t audio_radio_stop(void);
esp_err_t audio_radio_record_wav(const char *path, uint32_t duration_ms);
void audio_radio_deinit(void);
void *audio_radio_get_player(void);

#ifdef __cplusplus
}
#endif
