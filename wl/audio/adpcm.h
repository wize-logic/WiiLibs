// SPDX-License-Identifier: MIT
// ADPCM decoders - IMA-ADPCM (DS waves and streams); GameCube/Wii DSP-ADPCM (PWST, PWSF, BRSTM, DSP
// files) is the shared decoder in wl/codec/dsp_adpcm.h, included here. Integer only,
// platform-independent, host-tested.
#pragma once

#include "wl/base/base.h"

#include "wl/codec/dsp_adpcm.h" // the DSP-ADPCM decoder (WlDspState, wl_dsp_decode*) lives in wl/codec

// ---------------------------------------------------------------------------------------------
// IMA-ADPCM, the DS flavour: 4-bit samples, LOW nibble first, standard 89-entry step table. A block
// or wave carries its initial predictor and step index in a header the caller parses.
// ---------------------------------------------------------------------------------------------

typedef struct {
    int16_t pred;
    uint8_t index; // 0..88
} WlImaState;

// The step table (89 entries) and the index adjustments (by the low 3 bits of a nibble), for
// decoders that inline the step.
extern const int16_t wl_ima_step_table[89];
extern const int8_t wl_ima_index_table[8];

void wl_ima_init(WlImaState *st, int16_t pred, uint32_t index); // clamps index to 0..88
// Decodes one nibble (0..15) and returns the new sample.
int16_t wl_ima_step(WlImaState *st, uint32_t nibble);
// Decodes `samples` samples from `data` (low nibble first; odd counts leave the last high nibble
// unused).
void wl_ima_decode(WlImaState *st, const uint8_t *data, uint32_t samples, int16_t *out);
