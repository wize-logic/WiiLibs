// SPDX-License-Identifier: MIT
// GameCube/Wii DSP-ADPCM frame decoder used by the BRSTM and RWSD readers: a wrapper over the shared
// decoder in wl/codec/dsp_adpcm.h (included here). Integer only, the same result on every CPU.
//
// A frame is 8 bytes holding 14 samples. Header byte: predictor pair index (high nibble, 0..7) and
// scale exponent (low nibble); then 14 signed 4-bit samples, high nibble first:
//   s = clamp16((nibble * (2048 << scale) + 1024 + c0 * h1 + c1 * h2) >> 11)
// with h1, h2 the previous two output samples and (c0, c1) the coefficient pair the header selects
// (16 coefficients per channel, 8 pairs, units of 1/2048).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wl/base/base.h"
#include "wl/codec/dsp_adpcm.h"

/* Frames (8 bytes, 14 samples) needed for a sample count. */
#define WL_DSP_FRAMES(samples) (((samples) + 13u) / 14u)

/* Decodes nframes frames into out (nframes * 14 samples); *h1 (newest) and *h2 are the history and
 * are updated. A predictor index above 7 is decoded as index & 7 (check with
 * wl_brstm_dsp_frames_valid first when that matters). */
void wl_brstm_dsp_decode(const uint8_t *frames, size_t nframes, const int16_t coef[16], int16_t *h1,
                         int16_t *h2, int16_t *out);

/* WL_OK when every frame header has a predictor index of 0 to 7, else WL_ERR_FORMAT. */
WlResult wl_brstm_dsp_frames_valid(const uint8_t *frames, size_t nframes);
