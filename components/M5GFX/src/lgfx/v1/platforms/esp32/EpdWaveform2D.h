#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EPDiy-compatible 16-gray differential waveform format.
 *
 * Each phase contains 16 target-gray rows.  A row contains four bytes,
 * packing the two-bit drive values for all 16 source-gray levels from MSB
 * to LSB.  The phase payload size is therefore always 16 * 4 bytes.
 */
typedef struct {
    int phases;
    const uint8_t* luts;
    const int* phase_times;
} EpdWaveformPhases;

typedef struct {
    uint8_t type;
    uint8_t temp_ranges;
    EpdWaveformPhases const** range_data;
} EpdWaveformMode;

typedef struct {
    int min;
    int max;
} EpdWaveformTempInterval;

typedef struct {
    uint8_t num_modes;
    uint8_t num_temp_ranges;
    EpdWaveformMode const** mode_data;
    EpdWaveformTempInterval const* temp_intervals;
} EpdWaveform;

/*
 * The application supplies the panel-specific waveform.  M5GFX provides a
 * weak null implementation so other boards can still link without one.
 */
const EpdWaveform* lgfx_epd_2d_waveform_provider(void);

#ifdef __cplusplus
}
#endif
