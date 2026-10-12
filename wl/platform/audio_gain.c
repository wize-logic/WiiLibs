// SPDX-License-Identifier: MIT
// The audio output's master gain: see audio_gain.h.
#include "wl/platform/audio_gain.h"
#include "wl/platform/platform.h"

#include <string.h>

static unsigned bits_of(float v) {
    unsigned b;
    memcpy(&b, &v, sizeof b);
    return b;
}

static float float_of(unsigned b) {
    float v;
    memcpy(&v, &b, sizeof v);
    return v;
}

void wl_audio_gain_init(WlAudioGain *gain) {
    atomic_store(&gain->target_bits, bits_of(1.0f));
    atomic_store(&gain->ramp_ms, 0u);
    atomic_store(&gain->serial, 0u);
    gain->current = 1.0f;
    gain->target = 1.0f;
    gain->step = 0.0f;
    gain->left = 0;
    gain->seen = 0;
}

void wl_audio_gain_request(WlAudioGain *gain, float value, uint32_t ms) {
    if (!(value >= 0.0f)) {
        value = value < 0.0f ? 0.0f : 1.0f; // (NaN counts as untouched)
    }
    if (value > 1.0f) {
        value = 1.0f;
    }
    atomic_store(&gain->target_bits, bits_of(value));
    atomic_store(&gain->ramp_ms, ms);
    atomic_fetch_add_explicit(&gain->serial, 1u, memory_order_release);
}

float wl_audio_gain_requested(const WlAudioGain *gain) {
    return float_of(atomic_load(&gain->target_bits));
}

void wl_audio_gain_apply(WlAudioGain *gain, int16_t *stereo, uint32_t frames, uint32_t rate) {
    const unsigned serial = atomic_load_explicit(&gain->serial, memory_order_acquire);
    if (serial != gain->seen) {
        gain->seen = serial;
        gain->target = float_of(atomic_load(&gain->target_bits));
        const uint32_t ms = atomic_load(&gain->ramp_ms);
        const uint64_t ramp = (uint64_t)ms * rate / 1000u;
        if (ramp == 0u) {
            gain->current = gain->target;
            gain->left = 0;
        } else {
            gain->left = (uint32_t)ramp;
            gain->step = (gain->target - gain->current) / (float)ramp;
        }
    }
    if (gain->left == 0u && gain->current == 1.0f) {
        return;
    }
    for (uint32_t i = 0; i < frames; i++) {
        if (gain->left != 0u) {
            gain->current += gain->step;
            if (--gain->left == 0u) {
                gain->current = gain->target;
            }
        }
        const float g = gain->current;
        for (uint32_t c = 0; c < 2u; c++) {
            const float v = (float)stereo[2u * i + c] * g;
            stereo[2u * i + c] = (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
        }
    }
}

// ---- the output's own gain (platform.h)

static WlAudioGain g_master = {
    .target_bits = 0x3F800000u, // 1.0f
    .current = 1.0f,
    .target = 1.0f,
};

WlAudioGain *wl_audio_gain_master(void) {
    return &g_master;
}

void wl_platform_audio_fade(float gain, uint32_t ms) {
    wl_audio_gain_request(&g_master, gain, ms);
}

float wl_platform_audio_gain(void) {
    return wl_audio_gain_requested(&g_master);
}
