// SPDX-License-Identifier: MIT
// DSP-ADPCM frame decoder: a wrapper over wl/codec/dsp_adpcm.c (see dsp.h).
#include "wl/brstm/dsp.h"

#include <string.h>

void wl_brstm_dsp_decode(const uint8_t *frames, size_t nframes, const int16_t coef[16], int16_t *h1,
                         int16_t *h2, int16_t *out) {
    WlDspState st;
    memcpy(st.coef, coef, sizeof st.coef);
    st.h1 = *h1;
    st.h2 = *h2;
    for (size_t f = 0; f < nframes; f++) { // nframes is a size_t here; the shared call takes a uint32_t count
        wl_dsp_decode(&st, frames + f * WL_DSP_FRAME_BYTES, 1, out + f * WL_DSP_FRAME_SAMPLES);
    }
    *h1 = st.h1;
    *h2 = st.h2;
}

WlResult wl_brstm_dsp_frames_valid(const uint8_t *frames, size_t nframes) {
    for (size_t f = 0; f < nframes; f++) {
        if ((frames[f * 8u] >> 4) > 7u) {
            return WL_ERR_FORMAT;
        }
    }
    return WL_OK;
}
