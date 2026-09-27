/* Single translation unit for custom waveform LUT + MY_WAVEFORM table. */
#include "my_waveform.h"
#include "my_waveform_du.h"
#include "my_waveform_a2.h"
#include "my_waveform_du4.h"
#include "my_waveform_gc.h"
#include "my_waveform_gl.h"
#include "my_waveform_hd.h"

#include <string.h>

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

// Reproduce the E0470 project's default GC16 trim for diagnostic A/B testing:
// at most 11 erase phases, remove 5 black-saturation phases, then right-align
// each transition and retain 3 final idle phases. The source is our unchanged
// 48-phase GC16 table. This variant is never selected by the normal UI.
#define MY_GC16_TRIM_PHASES 36
#define MY_GC16_TRIM_ACTIVE 33
static uint8_t my_gc16_36_data[MY_GC16_TRIM_PHASES][16][4];
static const EpdWaveformPhases my_gc16_36_phases = {
    .phases = MY_GC16_TRIM_PHASES,
    .phase_times = NULL,
    .luts = (const uint8_t*)&my_gc16_36_data[0],
};
static const EpdWaveformPhases* my_gc16_36_ranges[] = { &my_gc16_36_phases };
static const EpdWaveformMode my_gc16_36_mode = {
    .type = 2, .temp_ranges = 1, .range_data = my_gc16_36_ranges,
};
static const EpdWaveformMode* my_gc16_36_modes[] = {
    &my_wm_du, &my_wm_a2, &my_wm_du4, &my_gc16_36_mode, &my_wm_gl, &my_wm_hd,
};
static const EpdWaveform my_gc16_36_waveform = {
    .num_modes = 6,
    .num_temp_ranges = 1,
    .mode_data = my_gc16_36_modes,
    .temp_intervals = my_waveform_intervals,
};

static unsigned gc16_action(unsigned phase, unsigned to, unsigned from)
{
    const uint8_t packed = my_gc_25_0_data[phase][to][from / 4];
    return (packed >> (6 - 2 * (from % 4))) & 3;
}

static void gc16_trim_pair(unsigned to, unsigned from,
                           uint8_t output[48], unsigned* length)
{
    uint8_t source[48];
    for (unsigned phase = 0; phase < 48; ++phase) {
        source[phase] = (uint8_t)gc16_action(phase, to, from);
    }
    unsigned first = 0;
    while (first < 48 && source[first] == 0) ++first;
    unsigned last = 48;
    while (last > first && source[last - 1] == 0) --last;
    *length = 0;
    if (first == last) return;

    const uint8_t saturation = to == 15 ? 2 : 1;
    const uint8_t erase = to == 15 ? 1 : 2;
    unsigned erase_count = 0;
    while (first + erase_count < last
        && source[first + erase_count] == erase) ++erase_count;
    unsigned saturation_count = 0;
    while (first + erase_count + saturation_count < last
        && source[first + erase_count + saturation_count] == saturation) {
        ++saturation_count;
    }

    const unsigned kept_erase = erase_count < 11 ? erase_count : 11;
    unsigned saturation_cut = to == 15 ? 0 : 5;
    if (saturation_cut >= saturation_count) {
        saturation_cut = saturation_count ? saturation_count - 1 : 0;
    }
    for (unsigned i = 0; i < kept_erase; ++i) output[(*length)++] = erase;
    for (unsigned i = 0; i < saturation_count - saturation_cut; ++i) {
        output[(*length)++] = saturation;
    }
    for (unsigned i = first + erase_count + saturation_count; i < last; ++i) {
        output[(*length)++] = source[i];
    }
}

const EpdWaveform* my_waveform_gc16_36(void)
{
    static int initialized = 0;
    static int valid = 0;
    if (initialized) return valid ? &my_gc16_36_waveform : NULL;

    uint8_t sequence[48];
    unsigned length;
    unsigned longest = 0;
    for (unsigned to = 0; to < 16; ++to) {
        for (unsigned from = 0; from < 16; ++from) {
            gc16_trim_pair(to, from, sequence, &length);
            if (length > longest) longest = length;
        }
    }
    if (longest == MY_GC16_TRIM_ACTIVE) {
        memset(my_gc16_36_data, 0, sizeof(my_gc16_36_data));
        for (unsigned to = 0; to < 16; ++to) {
            for (unsigned from = 0; from < 16; ++from) {
                gc16_trim_pair(to, from, sequence, &length);
                const unsigned start = longest - length;
                const unsigned shift = 6 - 2 * (from % 4);
                for (unsigned i = 0; i < length; ++i) {
                    my_gc16_36_data[start + i][to][from / 4] |=
                        (uint8_t)(sequence[i] << shift);
                }
            }
        }
        valid = 1;
    }
    initialized = 1;
    return valid ? &my_gc16_36_waveform : NULL;
}

const EpdWaveform* lgfx_epd_2d_waveform_provider(void)
{
    my_a2_init();
    my_du4_init();
    my_hd_init();
    return &MY_WAVEFORM;
}
