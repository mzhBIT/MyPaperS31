#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool connected;
    bool streaming;
    uint16_t vid;
    uint16_t pid;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
    uint8_t uac_version;
    bool hardware_volume;
    char product[48];
} usb_audio_output_status_t;

/** Install the board HS USB Host and UAC1/UAC2 class drivers. */
esp_err_t usb_audio_output_init(void);

/** Stop the active UAC stream but keep device enumeration alive. */
esp_err_t usb_audio_output_stop(void);

/** Start the connected headphone with an exactly supported PCM format. */
esp_err_t usb_audio_output_start(uint32_t sample_rate, uint8_t channels, uint8_t bits);

/** Return true when the connected headphone exposes the exact PCM format. */
bool usb_audio_output_supports(uint32_t sample_rate, uint8_t channels, uint8_t bits);

/** Pick an advertised rate for the requested channel/bit format. */
bool usb_audio_output_select_rate(uint32_t preferred_rate, uint32_t max_rate,
                                  uint8_t channels, uint8_t bits,
                                  uint32_t *selected_rate);

/** Select the closest supported USB PCM format. max_rate=0 means no ceiling. */
bool usb_audio_output_select_format(uint32_t source_rate, uint8_t source_channels,
                                    uint8_t source_bits, uint32_t max_rate,
                                    uint32_t *selected_rate, uint8_t *selected_channels,
                                    uint8_t *selected_bits);

/** Wait for a compatible UAC playback interface to be opened. */
bool usb_audio_output_wait_connected(uint32_t timeout_ms);

/** Copy PCM into the UAC transmit ring buffer. */
esp_err_t usb_audio_output_write(const uint8_t *data, size_t size, uint32_t timeout_ms);

/** Set normalized USB hardware volume when the device supports it. */
esp_err_t usb_audio_output_set_volume(uint8_t volume);

/** True only when PCM gain must be applied by the application. */
bool usb_audio_output_uses_software_volume(void);

/** Lock-free connection snapshot for UI/hot-plug polling. */
bool usb_audio_output_is_connected(void);

void usb_audio_output_get_status(usb_audio_output_status_t *status);

/** Stop the stream, close the device and release USB Host resources. */
void usb_audio_output_deinit(void);

/** True only after all Host/class tasks and DMA ownership are released. */
bool usb_audio_output_is_quiescent(void);

#ifdef __cplusplus
}
#endif
