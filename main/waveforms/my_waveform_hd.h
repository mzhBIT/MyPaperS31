#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lgfx/v1/platforms/esp32/EpdWaveform2D.h"

// Vendor Gray8 GL16 table: 30 equal-duration phases, 16x16 input transitions.
// It keeps the native 16-level source/target history while converging to an
// effective eight-gray ladder with GL16's reduced black/white flashing.
#define MY_HD_PHASE_COUNT 30
#define MY_WAVEFORM_MODE_HD 8

static uint8_t my_hd_25_0_data[MY_HD_PHASE_COUNT][16][4];
static bool my_hd_initialized = false;

static const char my_hd_table_b64[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAKgAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "gAAAAIAAAACqoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAIAAAACAAAAAgAAAAKgAAACoAAAAqqqAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAACAAAAAgAAAAIAAAACoAAAAqAAAAKgAAACqoAAAqqAAAKqqqgAAAAAA"
    "AAAAAAAAAAAAAAAAgAAAAIAAAACAAAAAqAAAAKgAAACoAAAAqqAAAKqgAACqoAAA"
    "qqqAAKqqgACqqqqgAAAAAAAAAACAAAAAgAAAAKgAAACoAAAAqAAAAKqgAACqoAAA"
    "qqAAAKqqgACqqoAAqqqAAKqqqgCqqqoAqqqqoAAAAAAAAAAAiAAAAKAAAACooAAA"
    "qiAAAKqAAACqooAAqqiAAKqqAACqqooAqqqiAKqqqACqqqogqqqqgKqqqqAAAAAA"
    "KAAAAIqgAACioAAAqKqAAKoqgACqioAAqqKqAKqoqgCqqioAqqqKoKqqoqCqqqig"
    "qqqqIKqqqoCqqqqgAAAAACqgAACKqoAAoqqAAKiqqgCqKqoAqoqqAKqiqqCqqKqg"
    "qqoqoKqqiqCqqqKgqqqooKqqqiiqqqqIqqqqoAAAAAAqqoAAiqqqAKKqqgCoqqqg"
    "qiqqoKqKqqCqoqqgqqiqoKqqKqCqqoqoqqqiqKqqqKiqqqooqqqqiFVVVVEAAAAE"
    "KqqqAIqqqqCiqqqgqKqqoKoqqqCqiqqgqqKqqKqoqqiqqiqoqqqKqKqqoqiqqqio"
    "qqqqKKqqqohVVVVRAAAABCqqqqCKqqqgoqqqoKiqqqiqKqqoqoqqqKqiqqiqqKqo"
    "qqoqqKqqiqiqqqKoqqqoqFVVVRVVVVVFVVVVUQAAAFQqqqqgiqqqqKKqqqioqqqo"
    "qiqqqKqKqqiqoqqoqqiqqKqqKqhVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVEAABVU"
    "KqqqqIqqqqiiqqqoqKqqqKoqqqiqiqqoVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRV"
    "VVVVFVVVVUVVVVVRAAVVVCqqqqiKqqqooqqqqFRVVVVVFVVVVUVVVVVRVVVVVFVV"
    "VVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUQFVVVQqqqqoRVVVVVFVVVVUVVVV"
    "VRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVEVVVVU"
    "FVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRV"
    "VVVVFVVVVUVVVVVRqqqqqBVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVV"
    "VVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUaqqqqgVVVVVRVVVVVFVVVVUVVVV"
    "VRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVGqqqqo"
    "FVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRV"
    "VVVVFVVVVUWqqqqiqqqqqBVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVV"
    "VVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFqqqqoqqqqqgVVVVVRVVVVVFVVVVUVVVV"
    "VRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFWqqqoqqqqqiqqqqqKqqqqo"
    "FVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVVVRVVqqqKqqqqoqqqqqiq"
    "qqqqKqqqqoqqqqqiqqqqqBVVVVVFVVVVUVVVVVRVVVVVFVVVVUVVVaqiqqqqqKqq"
    "qqoqqqqqiqqqqqKqqqqoqqqqqiqqqqqKqqqqoqqqqqgVVVVVRVVVVVFVVVWoqqqq"
    "qiqqqqqKqqqqoqqqqqiqqqqqKqqqqoqqqqqiqqqqqKqqqqoqqqqqiqqqqqKqqqqo"
    "FVVVVYqqqqqiqqqqqKqqqqoqqqqqiqqqqqKqqqqoqqqqqiqqqqqKqqqqoqqqqqiq"
    "qqqqKqqqqoqqqqqiqqqqqAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

static inline uint8_t my_hd_b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A');
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 52);
    return c == '+' ? 62 : 63;
}

static void my_hd_init(void)
{
    if (my_hd_initialized) return;
    uint8_t *output = &my_hd_25_0_data[0][0][0];
    size_t output_size = 0;
    for (size_t input = 0; my_hd_table_b64[input] != '\0'; input += 4) {
        uint32_t bits = ((uint32_t)my_hd_b64_value(my_hd_table_b64[input]) << 18)
                      | ((uint32_t)my_hd_b64_value(my_hd_table_b64[input + 1]) << 12)
                      | ((uint32_t)my_hd_b64_value(my_hd_table_b64[input + 2]) << 6)
                      |  (uint32_t)my_hd_b64_value(my_hd_table_b64[input + 3]);
        output[output_size++] = (uint8_t)(bits >> 16);
        output[output_size++] = (uint8_t)(bits >> 8);
        output[output_size++] = (uint8_t)bits;
    }
    my_hd_initialized = output_size == sizeof(my_hd_25_0_data);
}

static const EpdWaveformPhases my_hd_25_0 = {
    .phases = MY_HD_PHASE_COUNT,
    .phase_times = NULL,
    .luts = (const uint8_t *)&my_hd_25_0_data[0][0][0],
};
static const EpdWaveformPhases *my_wm_hd_ranges[1] = { &my_hd_25_0 };
static const EpdWaveformMode my_wm_hd = {
    .type = MY_WAVEFORM_MODE_HD,
    .temp_ranges = 1,
    .range_data = &my_wm_hd_ranges[0],
};
