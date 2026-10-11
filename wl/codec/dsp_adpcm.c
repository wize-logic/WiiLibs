// SPDX-License-Identifier: MIT
// DSP-ADPCM decoder (dsp_adpcm.h). Format: the public DSPADPCM layout.
#include "wl/codec/dsp_adpcm.h"

static int16_t clamp16(int64_t v) {
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void wl_dsp_decode_frame_from(WlDspState *st, uint8_t ps, const uint8_t frame[WL_DSP_FRAME_BYTES],
                              uint32_t first, int16_t out[WL_DSP_FRAME_SAMPLES]) {
    const uint32_t pair = (uint32_t)(ps >> 4) & 7u;
    const int64_t c0 = st->coef[2 * pair];
    const int64_t c1 = st->coef[2 * pair + 1];
    const int64_t step = (int64_t)2048 << (ps & 15u);
    int64_t h1 = st->h1;
    int64_t h2 = st->h2;
    for (uint32_t i = first; i < WL_DSP_FRAME_SAMPLES; i++) {
        const uint32_t byte = frame[1 + i / 2];
        int32_t nib = (int32_t)((i & 1u) ? (byte & 15u) : (byte >> 4));
        nib = nib >= 8 ? nib - 16 : nib;
        const int16_t s = clamp16((nib * step + 1024 + c0 * h1 + c1 * h2) >> 11);
        out[i] = s;
        h2 = h1;
        h1 = s;
    }
    if (first < WL_DSP_FRAME_SAMPLES) {
        st->h1 = (int16_t)h1;
        st->h2 = (int16_t)h2;
    }
}

void wl_dsp_decode(WlDspState *st, const uint8_t *frames, uint32_t nframes, int16_t *out) {
    for (uint32_t f = 0; f < nframes; f++) {
        const uint8_t *fr = frames + (size_t)f * WL_DSP_FRAME_BYTES;
        wl_dsp_decode_frame_from(st, fr[0], fr, 0, out + (size_t)f * WL_DSP_FRAME_SAMPLES);
    }
}
