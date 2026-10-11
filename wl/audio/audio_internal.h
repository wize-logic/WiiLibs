// SPDX-License-Identifier: MIT
// Shared between audio.c (engine, commands, samples) and stream.c (stream player). Not for users.
#pragma once

#include <stdatomic.h>

#include "wl/audio/audio.h"
#include "wl/platform/ring.h"

typedef enum {
    WL_ACMD_NOP = 0,
    WL_ACMD_PLAY_SAMPLE,
    WL_ACMD_PLAY_STREAM,
    WL_ACMD_STOP,
    WL_ACMD_SET,
    WL_ACMD_BUS,
    WL_ACMD_MASTER,
    WL_ACMD_DUCK,
    WL_ACMD_SPEECH_DUCK,
    WL_ACMD_STOP_ALL,
    WL_ACMD_STOP_PCM,
    WL_ACMD_PLAY_SYNTH // ptr = a WlSynth, pulled by the mixer
} WlAudioCmdOp;

typedef struct {
    uint32_t op;
    uint32_t id;
    const void *ptr; // pcm / stream
    uint32_t frames, loop_start, loop_end; // loop_end exclusive
    uint32_t rate;                         // source rate in Hz
    uint32_t fade_frames;
    int16_t pitch;                         // cents
    uint16_t gain;
    int8_t pan;
    uint8_t bus, priority, flags;
    uint32_t seq;
} WlAudioCmd;

#define WL_ACMD_FLAG_STEREO 1u
#define WL_ACMD_FLAG_LOOP 2u

// Pushes a command (main thread). Fills cmd->seq. False when the ring is full (counted).
bool wl_audio_push(WlAudioCmd *cmd);
// True while the mixer has (or may still have, a command being in flight) voice `id`.
bool wl_audio_voice_alive(uint32_t id);
// Exact: is the id in a mixer slot as of the last published block (ignores commands in flight).
bool wl_audio_voice_in_mixer(uint32_t id);
uint32_t wl_audio_cmds_done(void);
uint32_t wl_audio_cmds_issued(void);

// ---- stream player (stream.c) ----------------------------------------------------------------
WlResult wl_stream_start(const WlAudioSpan *span, const WlAudioPlay *play, uint32_t handle);
void wl_stream_pump(void);
bool wl_stream_handle_done(const WlFsDone *done);
void wl_stream_stop(uint32_t handle, uint32_t fade_ms);
// Freezes or resumes a stream (silence out, no data consumed). False for an unknown or closing stream.
bool wl_stream_set_paused(uint32_t handle, bool paused);
bool wl_stream_owns(uint32_t handle);
WlAudioState wl_stream_state(uint32_t handle);
void wl_stream_stats(uint32_t *starved, uint32_t *reads, uint32_t *errors, uint32_t *active);
// Drives streams to completion of any outstanding reads (shutdown). True when all are free.
bool wl_stream_idle(void);
// Forgets every stream and counter (engine start).
void wl_stream_reset(void);
// The mixer's pull callback for a stream voice (audio thread).
void wl_stream_pull(void *user, int16_t *stereo, uint32_t frames, bool *ended);
void wl_stream_stop_all(void);
