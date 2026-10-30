#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Own the shared Type-C HS data path and charger OTG supply. MSC runs in a
// separate boot mode and never calls these hooks. Release only after Host exits.
esp_err_t s31_usb_audio_port_acquire(void);
void s31_usb_audio_port_release(void);
bool s31_usb_audio_port_is_released(void);
#ifdef __cplusplus
}
#endif
