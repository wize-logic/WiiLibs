// SPDX-License-Identifier: MIT
// Shared by synth_data.c, synth_seq.c and synth_voice.c. Not for users.
#pragma once

#include "wl/audio/synth.h"

#define WL_SYN_TICK_UNIT 240u // tempo accumulator: a tick per 240 (tempo is added once per frame)
#define WL_SYN_RUNAWAY 4096u  // commands one track may run in a tick before it is cut off
#define WL_SYN_DEFAULT_TEMPO 120u
#define WL_SYN_DEFAULT_SEED 0x2545F491u

enum { WL_SYN_K_PCM8 = 0, WL_SYN_K_PCM16, WL_SYN_K_ADPCM, WL_SYN_K_PSG, WL_SYN_K_NOISE };

// synth_data.c
int32_t wl_syn_sin(uint32_t phase128); // -127..127, 128 steps per cycle

// synth_seq.c
void wl_syn_track_reset(WlSynTrack *t);
void wl_syn_tick(WlSynth *s);
void wl_syn_emit(WlSynth *s, uint32_t kind, uint32_t track, uint32_t voice, int32_t a, int32_t b,
                 int32_t c);

// synth_voice.c
void wl_syn_voice_release(WlSynth *s, uint32_t vi);
// Starts a voice for the note; returns its index, or -1 when no voice could be had.
int32_t wl_syn_voice_start(WlSynth *s, uint32_t track, const WlSynNote *nt, const WlSynWave *wave,
                           uint32_t key, uint32_t vel, int32_t len_ticks);
