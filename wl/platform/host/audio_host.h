// SPDX-License-Identifier: MIT
// Host stub of the audio output: no sound device. The test is the audio thread: it
// renders blocks with wl_host_audio_render, which calls the registered fill function exactly as the
// console's audio thread does, and can record or inspect the output. A second mode starts a real
// thread that renders blocks as fast as the machine allows (for race tests under ThreadSanitizer).
#pragma once

#include "wl/platform/platform.h"

// Calls the fill function for `frames` frames (rounded up to whole blocks; `out` must hold the
// rounded count, may be NULL to discard). Returns frames rendered (0 when not started or when
// the threaded mode is running).
uint32_t wl_host_audio_render(int16_t *out, uint32_t frames);

// Pretends the hardware ran dry n times (the underrun counter), for the code that reads it.
void wl_host_audio_inject_underruns(uint32_t n);

// Starts/stops a thread that calls `fill` back to back with a short sleep per block (about 1 ms,
// 8x real time), discarding the output. Needs the output to be started. For stress tests.
WlResult wl_host_audio_thread_start(void);
void wl_host_audio_thread_stop(void);
