// SPDX-License-Identifier: MIT
// Software mixer: 32 voices -> 4 buses -> master -> saturated PCM16
// stereo at 32 kHz in blocks of up to 256 frames. Integer only, platform-independent.
//
// Threading: every function runs on the one mixer (audio) thread; the main thread reaches it
// through the command ring of audio.h and never touches a WlMixer. No allocation, no locks.
//
// Gains are applied with a linear ramp across each block (no zipper noise): a voice's gain
// (volume, pan, fade) from the end of the previous block to this block's target; a bus's gain
// (bus volume, master, ducking) the same way. Mono sources are panned with a constant-power law,
// stereo sources get a balance control (centre = unity).
#pragma once

#include "wl/base/base.h"

#define WL_MIX_RATE 32000u
#define WL_MIX_BLOCK 256u        // frames in the largest chunk; ramps are per chunk
#define WL_MIX_MAX_VOICES 32u
#define WL_MIX_STEP_UNITY 65536u // pitch step in Q16 source frames per output frame
#define WL_MIX_STEP_MIN 1024u    // 1/64
#define WL_MIX_STEP_MAX 262144u  // 4.0 (two octaves up)

typedef enum { WL_BUS_MUSIC = 0, WL_BUS_EFFECTS, WL_BUS_SPEECH, WL_BUS_UI, WL_BUS_COUNT } WlBus;

// Pull source (streams): fill `frames` stereo interleaved frames completely (pad with silence when
// starved); set *ended when the source has nothing after these frames.
typedef void (*WlMixPullFn)(void *user, int16_t *stereo, uint32_t frames, bool *ended);

typedef struct {
    uint32_t id; // caller's voice id (non-zero); how the voice is found again
    uint8_t bus; // WlBus
    uint8_t priority; // higher wins when all voices are busy; 64 is the default
    uint16_t gain;    // Q8, 256 = 1.0 (clamped to 511)
    int8_t pan;       // -127..127 (balance for stereo sources)
    uint32_t rate;    // source sample rate in Hz (0 = 32000)
    int16_t pitch_cents; // pitch offset in cents (+-4800)
    uint32_t fade_in_frames; // 0 = none

    // Sample source (mono or stereo interleaved PCM16 in memory that outlives the voice) ...
    const int16_t *pcm;
    uint32_t frames;
    bool stereo;
    bool loop;
    uint32_t loop_start;
    uint32_t loop_end; // exclusive: the first frame after the loop (<= frames)
    // ... or a pull source (stereo).
    WlMixPullFn pull;
    void *pull_user;
} WlMixVoiceDesc;

typedef struct {
    uint32_t blocks;         // mix calls
    uint32_t frames;         // output frames produced
    uint32_t clipped;        // output samples that saturated
    uint32_t voices_started;
    uint32_t steals;         // a lower-priority voice was stopped for a new one
    uint32_t drops;          // a new voice found no free slot and no cheaper victim
    uint32_t active;         // voices active after the last block
    uint32_t peak;           // largest output magnitude in the last block (0..32768)
} WlMixStats;

typedef struct {
    uint32_t id;
    uint32_t seq; // start order, for stealing
    bool active;
    bool stopping;
    bool is_pull;
    bool stereo, loop, pull_ended;
    uint8_t bus, priority;
    uint16_t gain;
    int8_t pan;
    uint32_t rate;
    uint32_t step;
    const int16_t *pcm;
    uint32_t frames, loop_start, loop_end;
    uint32_t pos, frac;
    WlMixPullFn pull;
    void *pull_user;
    int16_t carry[4]; // up to 2 stereo frames of the pull window kept between blocks
    uint32_t carry_frames;
    int32_t gl, gr;  // Q14 gains at the end of the previous block
    int32_t fade;    // Q22, 1.0 = 16384 << 8
    int32_t fade_step; // Q22 per frame
    int32_t fade_goal; // Q22 value where fading stops (0 for a stop, 1.0 for a fade in)
} WlMixVoice;

typedef struct {
    WlMixVoice voices[WL_MIX_MAX_VOICES];
    uint16_t bus_gain[WL_BUS_COUNT]; // Q8
    uint16_t master;                 // Q8
    uint16_t duck_explicit;          // Q8 factor applied to the music bus (256 = none)
    uint16_t speech_duck;               // Q8 music factor while a speech-bus voice plays (default 128)
    int32_t bus_prev[WL_BUS_COUNT];  // Q14 effective bus gain at the end of the previous block
    int32_t duck_cur;                // Q14 current music duck factor
    uint32_t seq;
    WlMixStats stats;
    int32_t acc[WL_BUS_COUNT][2][WL_MIX_BLOCK];
    int16_t scratch[2 * (WL_MIX_BLOCK * 4 + 8)]; // pull window: carry + new stereo frames
} WlMixer;

void wl_mixer_init(WlMixer *m);

// Starts a voice. Returns the slot index, or -1 when the description is invalid or every slot is
// busy with equal or higher priority (counted in stats.drops). Stealing stops the lowest-priority,
// oldest voice instantly.
int wl_mixer_voice_start(WlMixer *m, const WlMixVoiceDesc *d);
bool wl_mixer_voice_active(const WlMixer *m, uint32_t id);
// Fades out over `fade_frames` (at least one block) and frees the slot. Unknown id: no-op.
void wl_mixer_voice_stop(WlMixer *m, uint32_t id, uint32_t fade_frames);
void wl_mixer_voice_set(WlMixer *m, uint32_t id, uint16_t gain, int8_t pan, int16_t pitch_cents);
// Frees every voice reading from `pcm` (before the sample memory is released).
void wl_mixer_stop_pcm(WlMixer *m, const int16_t *pcm);
void wl_mixer_stop_all(WlMixer *m);
void wl_mixer_bus_set(WlMixer *m, WlBus bus, uint16_t gain_q8);
void wl_mixer_master_set(WlMixer *m, uint16_t gain_q8);
void wl_mixer_duck_set(WlMixer *m, uint16_t music_factor_q8);

// Mixes `frames` (1..WL_MIX_BLOCK) stereo frames into `out` (interleaved), saturating to PCM16.
void wl_mixer_mix(WlMixer *m, int16_t *out, uint32_t frames);

// Q16 step for a source rate and a pitch offset in cents (clamped to the legal step range).
uint32_t wl_mixer_step(uint32_t source_rate, int32_t cents);
