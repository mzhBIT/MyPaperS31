#pragma once

#include "lgfx/v1/platforms/esp32/EpdWaveform2D.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const EpdWaveform MY_WAVEFORM;

// Diagnostic-only variant: GC16 is trimmed to 36 phases; all other modes
// continue to use the normal waveform tables. Returns NULL if generation fails.
const EpdWaveform* my_waveform_gc16_36(void);

#ifdef __cplusplus
}
#endif
