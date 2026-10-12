// SPDX-License-Identifier: MIT
// The audio output's master gain (platform.h, wl_platform_audio_fade): a ramp the audio thread applies
// to each block after the program's fill function, portable, so that both backends share it and the
// host tests run it.
#pragma once

#include <stdatomic.h>

#include "wl/base/base.h"

typedef struct {
    // the request (any thread): the last fade asked for; `serial` is written last
    atomic_uint target_bits; // the float's bits
    atomic_uint ramp_ms;
    atomic_uint serial;
    // the audio thread's
    float current;
    float target;
    float step;    // added per frame while `left` > 0
    uint32_t left; // frames of ramp left
    uint32_t seen; // the serial applied
} WlAudioGain;

void wl_audio_gain_init(WlAudioGain *gain);
// The fade asked for: `value` 0..1 (clamped, NaN as 1) over `ms`.
void wl_audio_gain_request(WlAudioGain *gain, float value, uint32_t ms);
// The gain asked for last.
float wl_audio_gain_requested(const WlAudioGain *gain);
// Audio thread: the block of `frames` stereo frames at `rate` Hz scaled by the ramp, which moves on.
// Returns at once when the gain is 1 and no ramp runs.
void wl_audio_gain_apply(WlAudioGain *gain, int16_t *stereo, uint32_t frames, uint32_t rate);

// The output's own (what wl_platform_audio_fade sets): the backends apply it to each block.
WlAudioGain *wl_audio_gain_master(void);
