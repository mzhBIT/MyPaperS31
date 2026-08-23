#pragma once

#include <stdint.h>

#include "audio_codec_ctrl_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create an esp_codec_dev control interface backed by M5Unified's internal
 * I2C bus. The address is the standard 7-bit I2C address.
 *
 * The returned interface is owned by the caller and can be released with
 * audio_codec_delete_ctrl_if(). Closing it never releases M5.In_I2C because
 * the EPD and touch controller share that bus.
 */
const audio_codec_ctrl_if_t *audio_codec_new_m5_i2c_ctrl(uint8_t port,
                                                         uint8_t address_7bit,
                                                         uint32_t frequency_hz);

#ifdef __cplusplus
}
#endif
