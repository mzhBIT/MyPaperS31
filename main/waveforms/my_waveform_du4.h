#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lgfx/v1/platforms/esp32/EpdWaveform2D.h"

/*
 * MODE_DU4 (type=7) -- experimental four-gray text waveform, 15 frames.
 *
 * The physical endpoints are 0, 5, 10 and 15.  The first implementation used
 * the net pulse count of the official GC16 transitions directly.  Panel tests
 * showed that this overdrives both middle levels because GC16's erase/rewrite
 * sequence is not equivalent to a continuous one-direction pulse train.
 *
 * DU4 therefore reuses the final 15-frame target-forming sequences from GC16
 * for gray 5 and gray 10.  At that point in the official waveform the rows are
 * source-independent and contain the required multi-step reverse corrections:
 * gray 5 = 111111210022000, gray 10 = 111110022222000.  Black/white endpoint
 * transitions retain the validated direct pulses. Transitions inside one
 * quantization bucket remain idle to prevent unchanged grays from walking,
 * except black->black which receives consolidation pulses.
 */

#define MY_DU4_PHASE_COUNT 15

static uint8_t my_du4_25_0_data[MY_DU4_PHASE_COUNT][16][4];

static const uint8_t my_du4_middle_sequence[2][MY_DU4_PHASE_COUNT] = {
    {1,1,1,1,1,1,2,1,0,0,2,2,0,0,0},
    {1,1,1,1,1,0,0,2,2,2,2,2,0,0,0},
};

// Signed direct endpoint pulse count indexed by source and target bucket.
// Only columns 0 and 3 are used. Positive is black; negative is white.
static const int8_t my_du4_endpoint_primary[4][4] = {
    {  0, 0, 0, -15},
    {  5, 0, 0, -10},
    { 10, 0, 0,  -5},
    { 15, 0, 0,   0},
};

static uint8_t my_du4_bucket(uint8_t gray)
{
    if (gray <= 2) return 0;
    if (gray <= 7) return 1;
    if (gray <= 12) return 2;
    return 3;
}

static void my_du4_init(void)
{
    static bool initialized;
    if (initialized) return;

    for (unsigned phase = 0; phase < MY_DU4_PHASE_COUNT; ++phase) {
        for (unsigned to = 0; to < 16; ++to) {
            const uint8_t to_bucket = my_du4_bucket((uint8_t)to);
            for (unsigned group = 0; group < 4; ++group) {
                uint8_t packed = 0;
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const uint8_t from = (uint8_t)(group * 4 + lane);
                    const uint8_t from_bucket = my_du4_bucket(from);
                    uint8_t drive = 0;
                    if (from_bucket == to_bucket) {
                        // Reinforce black->black at phase indices 10 and 11.
                        if (to_bucket == 0 && (phase == 10 || phase == 11)) {
                            drive = 1;
                        }
                    } else {
                        if (to_bucket == 1 || to_bucket == 2) {
                            drive = my_du4_middle_sequence[to_bucket - 1][phase];
                            if (from_bucket == 0 && to_bucket == 1 && phase == 7) {
                                drive = 2;
                            }
                            // A white start retained slightly too much of its
                            // old state. Add black before the terminal white
                            // correction so the last pulse does not dominate.
                            if (from_bucket == 3
                             && ((to_bucket == 1 && phase == 9)
                              || (to_bucket == 2 && phase == 6))) {
                                drive = 1;
                            }
                            // Fine adjustment after panel validation: both
                            // white-start middle levels remain slightly light.
                            // Remove the first terminal white correction rather
                            // than adding another full black pulse.
                            if (from_bucket == 3
                             && ((to_bucket == 1 && phase == 10)
                              || (to_bucket == 2 && phase == 7))) {
                                drive = 0;
                            }
                            // Photo-based fine calibration at 20 us/line:
                            // white->10 still measured slightly light, while
                            // black->5 and black->10 both need a small dark
                            // shift. Suppress one additional early white
                            // correction for only those three transitions.
                            if ((from_bucket == 3
                              && to_bucket == 2 && phase == 8)
                             || (from_bucket == 0
                              && to_bucket == 1 && phase == 10)
                             || (from_bucket == 0
                              && to_bucket == 2 && phase == 7)) {
                                drive = 0;
                            }
                        } else {
                            const int primary =
                                my_du4_endpoint_primary[from_bucket][to_bucket];
                            const unsigned count =
                                (unsigned)(primary < 0 ? -primary : primary);
                            if (phase >= MY_DU4_PHASE_COUNT - count) {
                                drive = primary > 0 ? 1 : (primary < 0 ? 2 : 0);
                            }
                        }
                    }
                    packed |= (uint8_t)(drive << (6 - lane * 2));
                }
                my_du4_25_0_data[phase][to][group] = packed;
            }
        }
    }
    initialized = true;
}

static const EpdWaveformPhases my_du4_25_0 = {
    .phases = MY_DU4_PHASE_COUNT,
    .luts = (const uint8_t*)&my_du4_25_0_data[0],
    .phase_times = NULL,
};
static const EpdWaveformPhases* my_wm_du4_ranges[1] = { &my_du4_25_0 };
static const EpdWaveformMode my_wm_du4 = {
    .type = 7,
    .temp_ranges = 1,
    .range_data = &my_wm_du4_ranges[0],
};
