// SPDX-License-Identifier: MIT
// IMA-ADPCM decoder (DSP-ADPCM is wl/codec/dsp_adpcm.c). The public IMA ADPCM
// description (89-entry step table, index adjustments -1 -1 -1 -1 2 4 6 8).
#include "wl/audio/adpcm.h"

// ---------------------------------------------------------------------------------------------
// IMA-ADPCM
// ---------------------------------------------------------------------------------------------

const int16_t wl_ima_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

const int8_t wl_ima_index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

void wl_ima_init(WlImaState *st, int16_t pred, uint32_t index) {
    st->pred = pred;
    st->index = (uint8_t)(index > 88u ? 88u : index);
}

int16_t wl_ima_step(WlImaState *st, uint32_t nibble) {
    const int32_t step = wl_ima_step_table[st->index];
    int32_t diff = step >> 3;
    if (nibble & 4u) {
        diff += step;
    }
    if (nibble & 2u) {
        diff += step >> 1;
    }
    if (nibble & 1u) {
        diff += step >> 2;
    }
    int32_t p = st->pred + ((nibble & 8u) ? -diff : diff);
    p = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    int32_t idx = (int32_t)st->index + wl_ima_index_table[nibble & 7u];
    idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
    st->pred = (int16_t)p;
    st->index = (uint8_t)idx;
    return st->pred;
}

void wl_ima_decode(WlImaState *st, const uint8_t *data, uint32_t samples, int16_t *out) {
    for (uint32_t i = 0; i < samples; i++) {
        const uint32_t byte = data[i / 2];
        out[i] = wl_ima_step(st, (i & 1u) ? (byte >> 4) : (byte & 15u));
    }
}
