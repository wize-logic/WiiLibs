// SPDX-License-Identifier: MIT
// Synthetic audio content for the audio engine tests: a deterministic integer melody, encoded as
// PWST streams and PWSF samples (layouts in wl/audio/README.md). Nothing here comes from game
// audio. Built on the DSP-ADPCM encoder of dsp_fixture.h.
#pragma once

#include "wl/base/base.h"

typedef struct {
    uint32_t rate;
    uint32_t samples; // per channel, a multiple of 14
    uint8_t channels; // 1 or 2
    bool loop;
    uint32_t loop_start; // loops from here; a loop_start that is not a multiple of 14 is mid-frame
    uint32_t loop_end;   // inclusive
    uint32_t melody;     // selects the tune
} FxAStreamSpec;

// A short pentatonic arpeggio, 4 notes per second, per-channel variation. Position-unique enough to
// catch any misalignment. `n` samples at `rate`.
void fxa_tone(int16_t *out, uint32_t n, uint32_t rate, uint32_t melody, uint32_t channel);

// Builds a PWST payload (malloc; caller frees). `ref` (may be NULL) receives, per channel, the
// samples a correct decoder produces (malloc; caller frees).
WlResult fxa_build_pwst(const FxAStreamSpec *spec, uint8_t **out, size_t *size, int16_t *ref[2]);

typedef struct {
    uint32_t rate;
    uint32_t samples;
    uint8_t codec; // 0 DSP-ADPCM, 1 PCM16
    uint8_t bus;
    uint8_t priority;
    bool loop;
    uint32_t loop_start, loop_end; // inclusive end
    int16_t pitch_cents;
    uint16_t volume; // 256 = 1.0
    int8_t pan;
    uint16_t play_ms;
    uint32_t melody;
} FxASfxSpec;

WlResult fxa_build_pwsf(const FxASfxSpec *spec, uint8_t **out, size_t *size, int16_t **ref);
