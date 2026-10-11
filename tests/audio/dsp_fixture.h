// SPDX-License-Identifier: MIT
// Synthetic DSP-ADPCM material for the audio tests: an integer test tone, a reference encoder and
// decoder written straight from the frame layout (wl/audio/adpcm.h), and a coefficient set of our
// own. Nothing here comes from game audio.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Deterministic integer-only test tone: `n` samples of a 64-sample-period sine, amplitude 12000.
void fx_sine_pcm(int16_t *out, size_t n);

// Encodes n samples (multiple of 14) into n / 14 * 8 bytes with the 8 coefficient pairs `coef`
// (brute force over predictor and scale per frame); history starts at zero.
void fx_dsp_encode(const int16_t *pcm, size_t n, const int16_t coef[16], uint8_t *out);
// Decodes `frames` frames; `hist` is {hist1, hist2} in/out.
void fx_dsp_decode(const uint8_t *data, size_t frames, const int16_t coef[16], int16_t *out,
                   int16_t hist[2]);
// Eight predictor pairs in units of 1/2048.
extern const int16_t FX_DSP_COEF[16];
