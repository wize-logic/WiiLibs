// SPDX-License-Identifier: MIT
// The GameCube/Wii DSP-ADPCM decoder, shared by wl/audio (stream and sample players) and wl/brstm
// (BRSTM, RWSD). Integer only, portable.
//
// 8-byte frames, 14 samples each. Header byte: predictor pair index (high nibble, 0..7 used), scale exponent
// (low nibble); then 14 signed 4-bit samples, high nibble first.
//   s = clamp16((nibble * (2048 << scale) + 1024 + c0 * h1 + c1 * h2) >> 11)
#pragma once

#include <stddef.h>
#include <stdint.h>

#define WL_DSP_FRAME_BYTES 8u
#define WL_DSP_FRAME_SAMPLES 14u

typedef struct {
    int16_t coef[16]; // 8 pairs (c0, c1), units of 1/2048
    int16_t h1, h2;   // the previous two output samples (h1 the newest)
} WlDspState;

// Decodes nibbles first..13 of `frame` into out[first..13] (out[0..first-1] untouched), advancing
// the history. `ps` is the header byte to use (normally frame[0]; a loop restart passes the
// channel block's loop_ps). first > 13 is ignored.
void wl_dsp_decode_frame_from(WlDspState *st, uint8_t ps, const uint8_t frame[WL_DSP_FRAME_BYTES],
                              uint32_t first, int16_t out[WL_DSP_FRAME_SAMPLES]);

// Decodes `nframes` whole frames (14 * nframes samples) with the headers taken from the data.
void wl_dsp_decode(WlDspState *st, const uint8_t *frames, uint32_t nframes, int16_t *out);
