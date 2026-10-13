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

// ---------------------------------------------------------------------------------------------
// Xbox ADPCM (format tag 0x0069): IMA-ADPCM in Microsoft's block layout, 36 bytes per channel per
// block. Each channel's 4-byte header comes first (s16 little-endian predictor, which is the
// block's first sample; the step index as a signed byte; a reserved byte), then groups of 4 bytes
// per channel in turn, each 8 nibbles (8 samples), low nibble first. A block decodes to 65
// samples per channel (the header's and 64 nibbles'); the Xbox counts 64 per block (DirectSound's
// lengths and the APU's sample offsets), so a player takes the header sample and the first 63
// nibbles and the last nibble's sample is the next block's header again.
// The reserved byte is not checked (titles end buffers with other values in it), and the index
// byte is taken as signed and clamped to 0..88 (a byte of 200 is -56, so 0).
// ---------------------------------------------------------------------------------------------

#define WL_XBOX_ADPCM_BLOCK 36u          // bytes per channel per block
#define WL_XBOX_ADPCM_BLOCK_SAMPLES 64u  // samples per block as the Xbox counts them

// Starts channel `ch`'s decoder from a block's header (`block` points at the block's first byte)
// and returns the header sample.
int16_t wl_xbox_adpcm_header(const uint8_t *block, uint32_t ch, WlImaState *st);
// Nibble k (0..63) of channel ch in a block of `channels` channels.
static inline uint32_t wl_xbox_adpcm_nibble(const uint8_t *block, uint32_t channels, uint32_t ch, uint32_t k) {
    const uint32_t byte = block[4u * channels * (1u + k / 8u) + 4u * ch + (k % 8u) / 2u];
    return (k & 1u) ? byte >> 4 : byte & 15u;
}
// Decodes one block of 1 or 2 channels into 65 samples per channel, interleaved (out[i * channels
// + ch]). `block` holds WL_XBOX_ADPCM_BLOCK * channels bytes.
void wl_xbox_adpcm_decode_block(const uint8_t *block, uint32_t channels, int16_t *out);
