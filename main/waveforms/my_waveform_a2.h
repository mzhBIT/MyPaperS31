#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lgfx/v1/platforms/esp32/EpdWaveform2D.h"

/*
 * MODE_A2 (type=4) -- fourteen-frame binary waveform for epd_fastest.
 *
 * Frames 0..12 drive directly toward the thresholded black/white endpoint;
 * frame 13 is idle so the panel is not powered down on an active drive frame.
 * Logical levels 0..7 map to black and 8..15 map to white.
 */

#define MY_A2_PHASE_COUNT 14

static uint8_t my_a2_25_0_data[MY_A2_PHASE_COUNT][16][4];

static void my_a2_init(void)
{
    static bool initialized;
    if (initialized) return;

    for (unsigned phase = 0; phase < MY_A2_PHASE_COUNT; ++phase) {
        for (unsigned to = 0; to < 16; ++to) {
            const bool target_white = to >= 8;
            for (unsigned group = 0; group < 4; ++group) {
                uint8_t packed = 0;
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const uint8_t from = (uint8_t)(group * 4 + lane);
                    uint8_t drive = 0;
                    if (phase < MY_A2_PHASE_COUNT - 1) {
                        if (target_white) {
                            if (from < 15) drive = 2;
                        } else {
                            if (from > 0) drive = 1;
                        }
                    }
                    packed |= (uint8_t)(drive << (6 - lane * 2));
                }
                my_a2_25_0_data[phase][to][group] = packed;
            }
        }
    }
    initialized = true;
}

static const EpdWaveformPhases my_a2_25_0 = {
    .phases = MY_A2_PHASE_COUNT,
    .luts = (const uint8_t*)&my_a2_25_0_data[0],
    .phase_times = NULL,
};
static const EpdWaveformPhases* my_wm_a2_ranges[1] = { &my_a2_25_0 };
static const EpdWaveformMode my_wm_a2 = {
    .type = 4,
    .temp_ranges = 1,
    .range_data = &my_wm_a2_ranges[0],
};
