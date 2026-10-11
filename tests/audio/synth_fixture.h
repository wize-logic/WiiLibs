// SPDX-License-Identifier: MIT
// Synthetic SSEQ, SBNK and SWAR files for the SSEQ synth tests: built from our own byte layouts
// (the SDAT sub-file layouts summarised in wl/audio/README.md), integer only, no game data.
// Compiled for the host tests and the big-endian tests.
#pragma once

#include "wl/base/base.h"

// SSEQ file around `cmds` (header 0x1C bytes, DATA block). Returns the file size, 0 when `cap` is
// too small.
size_t synfix_sseq(uint8_t *out, size_t cap, const uint8_t *cmds, size_t n);

typedef struct {
    uint8_t enc; // 0 PCM8, 1 PCM16, 2 IMA-ADPCM
    bool loop;
    uint16_t rate;
    uint32_t loop_start_w; // 32-bit words (ADPCM counts its header word)
    uint32_t loop_len_w;
    const uint8_t *data; // (loop_start_w + loop_len_w) * 4 bytes, as stored
} SynfixWave;

// SWAR file with `n` waves. Returns the size, 0 on overflow.
size_t synfix_swar(uint8_t *out, size_t cap, const SynfixWave *w, uint32_t n);

typedef struct {
    uint8_t type;                                      // 1 PCM, 2 PSG, 3 noise
    uint16_t wave;                                     // SWAV index, or the PSG duty
    uint16_t slot;                                     // wave archive slot
    uint8_t key, attack, decay, sustain, release, pan; // pan 64 = centre
} SynfixNote;

typedef struct {
    uint8_t kind; // 0 empty, 1 single (notes[0]), 16 drum set (keys lo..hi), 17 key split
                  // (split_end, 8 regions at most)
    uint8_t lo, hi;
    uint8_t split_end[8];
    uint8_t count; // notes used
    SynfixNote notes[8];
} SynfixInst;

// SBNK file with the given programs. Returns the size, 0 on overflow.
size_t synfix_sbnk(uint8_t *out, size_t cap, const SynfixInst *inst, uint32_t n);

// A one-note instrument.
SynfixInst synfix_single(uint8_t type, uint16_t wave, uint8_t key, uint8_t a, uint8_t d, uint8_t s,
                     uint8_t r);

// Sawtooth-ish test waves: sample i = ((i * 7) mod period) scaled, non-trivial and position-unique.
void synfix_wave16(int16_t *out, uint32_t n, uint32_t seed);

// IMA-ADPCM (DS flavour) encoding of `n` samples (multiple of 8): the 4-byte header (initial
// predictor 0, index 0) then the nibbles, low first. Returns the byte count (4 + n / 2). `ref` (may
// be NULL) receives the samples a correct decoder produces.
size_t synfix_ima_encode(const int16_t *pcm, uint32_t n, uint8_t *out, int16_t *ref);
