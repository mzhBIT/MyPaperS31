/* Single translation unit for custom waveform LUT + MY_WAVEFORM table. */
#include "my_waveform.h"
#include "my_waveform_du.h"
#include "my_waveform_a2.h"
#include "my_waveform_du4.h"
#include "my_waveform_gc.h"
#include "my_waveform_gl.h"
#include "my_waveform_hd.h"

static const EpdWaveformMode* my_waveform_modes[] = {
    &my_wm_du,
    &my_wm_a2,
    &my_wm_du4,
    &my_wm_gc,
    &my_wm_gl,
    &my_wm_hd,
};

static const EpdWaveformTempInterval my_waveform_intervals[] = {
    { .min = 0, .max = 50 },
};

const EpdWaveform MY_WAVEFORM = {
    .num_modes = 6,
    .num_temp_ranges = 1,
    .mode_data = my_waveform_modes,
    .temp_intervals = my_waveform_intervals,
};

const EpdWaveform* lgfx_epd_2d_waveform_provider(void)
{
    my_a2_init();
    my_du4_init();
    my_hd_init();
    return &MY_WAVEFORM;
}
