// SPDX-License-Identifier: MIT
// Synthetic DSP-ADPCM material for the audio tests (see dsp_fixture.h).
#include "tests/audio/dsp_fixture.h"

#include <string.h>

void fx_sine_pcm(int16_t *out, size_t n) {
    // Rotation by 2*pi/64 in Q30; integer only, so the bytes do not depend on libm.
    const int64_t c = 1068571464, s = 105245103;
    int64_t x = 12000, y = 0;
    for (size_t i = 0; i < n; i++) {
        out[i] = (int16_t)y;
        int64_t nx = (x * c - y * s) >> 30;
        int64_t ny = (x * s + y * c) >> 30;
        x = nx;
        y = ny;
    }
}

// Eight predictor pairs in units of 1/2048 (our own set; any pairs are legal).
const int16_t FX_DSP_COEF[16] = {0,    0,    1024, 0,     1536, 0,     2048, 0,
                                 2048, -1024, 3072, -1024, 3584, -1536, 4096, -2048};

static int16_t clamp16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static int32_t decode_one(int nib, int scale, const int16_t *c, int32_t h1, int32_t h2) {
    int32_t v = (nib * (2048 << scale) + 1024 + c[0] * h1 + c[1] * h2) >> 11;
    return clamp16(v);
}

void fx_dsp_encode(const int16_t *pcm, size_t n, const int16_t coef[16], uint8_t *out) {
    int32_t h1 = 0, h2 = 0;
    for (size_t f = 0; f < n / 14; f++) {
        int64_t best_err = -1;
        uint8_t best[8] = {0};
        int32_t best_h1 = 0, best_h2 = 0;
        for (int p = 0; p < 8; p++) {
            for (int sc = 0; sc < 12; sc++) {
                int32_t a = h1, b = h2;
                int64_t err = 0;
                uint8_t fr[8] = {(uint8_t)((p << 4) | sc), 0, 0, 0, 0, 0, 0, 0};
                for (int i = 0; i < 14; i++) {
                    int32_t s = pcm[f * 14 + (size_t)i];
                    int32_t pred = coef[2 * p] * a + coef[2 * p + 1] * b;
                    int32_t step = 2048 << sc;
                    int32_t t = s * 2048 - pred + step / 2;
                    int32_t q = t >= 0 ? t / step : -((-t + step - 1) / step); // floor
                    q = q > 7 ? 7 : q < -8 ? -8 : q;
                    int32_t d = decode_one(q, sc, &coef[2 * p], a, b);
                    err += (int64_t)(d - s) * (d - s);
                    b = a;
                    a = d;
                    fr[1 + i / 2] |= (uint8_t)((q & 15) << (i % 2 ? 0 : 4));
                }
                if (best_err < 0 || err < best_err) {
                    best_err = err;
                    memcpy(best, fr, 8);
                    best_h1 = a;
                    best_h2 = b;
                }
            }
        }
        memcpy(out + f * 8, best, 8);
        h1 = best_h1;
        h2 = best_h2;
    }
}

void fx_dsp_decode(const uint8_t *data, size_t frames, const int16_t coef[16], int16_t *out,
                   int16_t hist[2]) {
    int32_t h1 = hist[0], h2 = hist[1];
    for (size_t f = 0; f < frames; f++) {
        const uint8_t *fr = data + f * 8;
        int p = fr[0] >> 4, sc = fr[0] & 15;
        for (int i = 0; i < 14; i++) {
            int nib = (fr[1 + i / 2] >> (i % 2 ? 0 : 4)) & 15;
            nib = nib >= 8 ? nib - 16 : nib;
            int32_t d = decode_one(nib, sc, &coef[2 * (p & 7)], h1, h2);
            out[f * 14 + (size_t)i] = (int16_t)d;
            h2 = h1;
            h1 = d;
        }
    }
    hist[0] = (int16_t)h1;
    hist[1] = (int16_t)h2;
}
