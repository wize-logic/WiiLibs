// SPDX-License-Identifier: MIT
// SSEQ synth (written from community format notes: SSEQ commands, SBNK instruments, SWAR/SWAV
// waves; see wl/audio/README.md): a
// software re-implementation of the DS sequence player. One WlSynth plays one sequence on its own
// pool of up to 32 voices and renders 32 kHz stereo PCM16. Integer only, no allocation, no I/O,
// platform independent: the output is bit-identical on the host and on the (big-endian) console.
//
// Ownership: the SSEQ, SBNK and SWAR bytes are the caller's and must outlive the WlSynth (they stay
// little-endian as stored; every read goes through byte accessors). Threading: a WlSynth is driven
// from one thread (the audio thread, through wl_syn_pull or wl_syn_render); only the functions
// marked "any thread" may be called while it plays.
//
// Time. The engine runs at 192 Hz ("frames", 166.67 samples at 32 kHz; a Bresenham accumulator
// keeps the frame lengths integral). A tick is 1/48 of a quarter note: the tempo accumulator adds
// the tempo (BPM) per frame and a tick fires for every 240, so tempo 120 gives 96 ticks per second.
// Envelopes, LFOs, pitch and gains are recomputed per frame; gains ramp linearly across it.
#pragma once

#include <stdatomic.h>

#include "wl/audio/adpcm.h"
#include "wl/base/base.h"

#define WL_SYN_RATE 32000u
#define WL_SYN_FRAME_HZ 192u
#define WL_SYN_TRACKS 16u
#define WL_SYN_VOICES 32u
#define WL_SYN_VARS 32u   // 0..15 belong to the player, 16..31 are shared (global)
#define WL_SYN_STACK 3u   // nesting of calls and of loops per track
#define WL_SYN_CHUNK 256u // frames rendered per internal pass
#define WL_SYN_ENV_MIN                                                                             \
    (-92544) // envelope units: 1/128 of a tenth of a decibel; -723 dB10 is silence

// ---- parsed data (views into the caller's bytes)
// ------------------------------------------------------

typedef struct {
    const uint8_t *file;
    size_t size;
    const uint8_t *data; // the command bytes (jump targets are offsets into this)
    size_t data_size;
} WlSynSeq;

// Validates the SSEQ header (magic, size, DATA block, data offset).
WlResult wl_syn_seq_open(WlSynSeq *q, const uint8_t *sseq, size_t n);

typedef struct {
    const uint8_t *file;
    size_t size;
    uint32_t count;
} WlSynWaveArc;

// Validates the SWAR header and offset table (individual waves are checked when used).
WlResult wl_syn_waves_open(WlSynWaveArc *w, const uint8_t *swar, size_t n);

typedef struct {
    const uint8_t *file;
    size_t size;
    uint32_t count;
    WlSynWaveArc war[4]; // the bank's four wave archive slots; file == NULL = empty
} WlSynBank;

// Validates the SBNK header and record table. Instruments are checked when looked up.
WlResult wl_syn_bank_open(WlSynBank *b, const uint8_t *sbnk, size_t n);
// slot 0..3; NULL clears it.
void wl_syn_bank_set_waves(WlSynBank *b, uint32_t slot, const WlSynWaveArc *w);

typedef enum {
    WL_SYN_NOTE_NONE = 0,
    WL_SYN_NOTE_PCM = 1,  // a wave (PCM8, PCM16 or IMA-ADPCM) of the SWAR in slot `slot`
    WL_SYN_NOTE_PSG = 2,  // pulse wave; `wave` is the duty (0 = 1/8 ... 6 = 7/8)
    WL_SYN_NOTE_NOISE = 3 // 15-bit noise
} WlSynNoteType;

typedef struct {
    uint8_t type;  // WlSynNoteType
    uint8_t slot;  // wave archive slot 0..3
    uint16_t wave; // SWAV index, or the PSG duty
    uint8_t key;   // the key the wave sounds at its recorded rate (the base key)
    uint8_t attack, decay, sustain, release; // 0..127 (a stored 255 is read as 127)
    int8_t pan;                              // -64..63
} WlSynNote;

// The note definition program `prog` plays for `key`: a single instrument, the drum set entry of
// the key, or the key-split region that contains it. False for an empty or missing program or a key
// outside a drum set or split.
bool wl_syn_bank_note(const WlSynBank *b, uint32_t prog, uint32_t key, WlSynNote *out);

typedef struct {
    uint8_t enc; // 0 PCM8, 1 PCM16, 2 IMA-ADPCM
    bool loop;
    uint16_t rate;
    uint32_t nsamples;
    uint32_t loop_start; // sample index (ADPCM: the 4-byte header does not count)
    const uint8_t *data; // ADPCM: starts at the 4-byte header
} WlSynWave;

// Wave `idx` of an archive (empty slot, bad index, bad extents: false).
bool wl_syn_wave_get(const WlSynWaveArc *w, uint32_t idx, WlSynWave *out);

typedef struct {
    uint32_t instruments; // non-empty programs
    uint32_t notes;       // note definitions
    uint32_t pcm_notes;
    uint32_t unresolved; // PCM notes whose wave archive slot or wave is missing or bad
    uint32_t bad;        // instruments that do not parse
} WlSynBankCheck;
// Looks at every instrument and PCM note of the bank (tools and tests).
WlResult wl_syn_bank_check(const WlSynBank *b, WlSynBankCheck *out);

// ---- the player
// ---------------------------------------------------------------------------------------

typedef struct {
    uint8_t volume;        // the sequence's volume from the archive (SDAT INFO), 0..127; 127
    uint8_t chan_priority; // added to the track priority to rank voices; 0
    uint8_t voice_limit;   // 1..32; 0 = 32
    bool nearest;          // "DS-crisp": no interpolation between wave samples
    uint16_t tempo_q8;     // tempo scale, 256 = 1.0
    uint16_t gain_q8;      // output gain, 256 = 1.0
    uint32_t seed;         // random command seed; 0 = fixed default
    int16_t *global_vars;  // WL_SYN_VARS - 16 shared variables (NULL = private ones)
} WlSynParams;

void wl_syn_params_default(WlSynParams *p);

typedef enum {
    WL_SYN_V_OFF = 0,
    WL_SYN_V_ATTACK,
    WL_SYN_V_DECAY,
    WL_SYN_V_SUSTAIN,
    WL_SYN_V_RELEASE
} WlSynVoiceState;

typedef struct {
    uint8_t state; // WlSynVoiceState
    uint8_t kind;  // internal wave kind
    uint8_t track, key, vel, prio, duty, base_key;
    bool loop, loop_saved, tied, held;
    int8_t inst_pan;
    uint8_t attack_mult;
    uint32_t order; // start order, for stealing
    int32_t env;    // WL_SYN_ENV_MIN..0
    int32_t sustain_env, decay_rate, release_rate;
    int32_t len;         // ticks until release (0 with state < RELEASE: release now)
    int32_t base_pitch;  // 1/64 semitones: (key - base key) * 64, before bends and modulation
    int32_t sweep_pitch; // starting offset of the pitch glide, 1/64 semitones
    uint32_t sweep_start_tick;
    uint32_t sweep_frames; // glide length in frames; 0 = the note's length in ticks
    uint32_t sweep_ticks;
    uint32_t age_frames;
    uint32_t lfo_delay_cnt, lfo_phase;
    uint32_t rate;
    uint32_t nsamp, loop_start;
    const uint8_t *data;
    uint32_t rd;   // index of the next sample to fetch
    uint32_t tail; // fetches past the end of a one-shot wave
    int32_t s0, s1;
    uint32_t frac, step;
    WlImaState ima, ima_loop;
    uint16_t lfsr;
    int32_t gl, gr, dgl, dgr; // Q20 gains now and per sample
    int32_t tgl, tgr;         // Q20 gains this frame's ramp ends at
} WlSynVoice;

typedef struct {
    bool active;
    bool notewait, tie, porta;
    uint16_t prog;
    uint8_t vol, expr, bend_range, prio;
    int8_t pan, bend, transpose;
    uint8_t porta_key, porta_time;
    uint8_t mod_depth, mod_speed, mod_type, mod_range;
    uint16_t mod_delay;
    uint8_t atk, dec, sus, rel; // 255 = use the instrument's
    int16_t sweep;
    bool cond;
    int32_t wait;
    uint32_t pc;
    uint8_t call_depth, loop_depth;
    uint32_t call_pc[WL_SYN_STACK];
    uint32_t loop_pc[WL_SYN_STACK];
    uint8_t loop_cnt[WL_SYN_STACK];
    int8_t tie_voice; // voice index of the held note, -1 none
    uint32_t tie_order;
} WlSynTrack;

typedef struct {
    uint32_t ticks, frames;
    uint32_t notes;         // note-on commands that reached the voices
    uint32_t notes_silent;  // no instrument or wave for them
    uint32_t notes_dropped; // no voice free and none cheaper to steal
    uint32_t steals;
    uint32_t steals_live; // of those, voices that had not been released yet (an audible cut)
    uint32_t ties;        // notes that continued a held note
    uint32_t voices_peak;
    uint32_t unknown_cmds; // commands this synth does not know (the track ends)
    uint8_t last_unknown;
    uint32_t runaway;       // tracks cut off after too many commands without a wait
    uint32_t clipped;       // output samples that saturated
    uint64_t voice_samples; // voice-samples mixed (CPU cost accounting)
} WlSynStats;

typedef enum {
    WL_SYN_EV_TICK = 1,  // a = tempo
    WL_SYN_EV_NOTE_ON,   // track, voice, a = key, b = velocity, c = length in ticks
    WL_SYN_EV_RELEASE,   // track, voice (the note's length ran out)
    WL_SYN_EV_VOICE_OFF, // voice (the envelope ended or the wave ended)
    WL_SYN_EV_STEAL,     // voice (stolen for a newer note)
    WL_SYN_EV_TRACK_END, // track
    WL_SYN_EV_PROGRAM    // track, a = program
} WlSynEventKind;

typedef struct {
    uint8_t kind, track, voice;
    uint32_t tick;
    int32_t a, b, c;
} WlSynEvent;
typedef void (*WlSynTraceFn)(void *user, const WlSynEvent *ev);

typedef struct WlSynth {
    WlSynSeq seq;
    const WlSynBank *bank;
    WlSynParams par;
    int16_t vars[WL_SYN_VARS];
    int16_t *gvars; // the 16 shared variables
    uint32_t rng;
    uint16_t tempo; // BPM, E1
    uint8_t master; // C2
    uint32_t tempo_acc;
    uint32_t tick;
    uint32_t frame_acc;  // Bresenham accumulator for the frame length
    uint32_t frame_left; // samples left in the current frame; 0 = run the next frame
    uint32_t frame_len;
    uint32_t voice_order;
    bool done;
    atomic_bool paused;
    WlSynStats stats;
    WlSynTraceFn trace;
    void *trace_user;
    uint32_t *op_hist; // NULL, or 256 counters of the commands executed (tools: command census)
    WlSynTrack trk[WL_SYN_TRACKS];
    WlSynVoice voice[WL_SYN_VOICES];
    int32_t acc[2 * WL_SYN_CHUNK];
} WlSynth;

// Prepares to play `seq` on `bank` from its first command (track 0). The bank and seq structures
// must outlive the player. WL_ERR_ARG, WL_ERR_FORMAT (empty sequence).
WlResult wl_syn_init(WlSynth *s, const WlSynSeq *seq, const WlSynBank *bank,
                     const WlSynParams *par);

// Renders `frames` stereo frames (interleaved) and advances the sequencer; any number of frames.
// After the sequence has ended and its voices have died the output is silence and wl_syn_done is
// true.
void wl_syn_render(WlSynth *s, int16_t *stereo, uint32_t frames);
// The pull function the audio engine's mixer calls (wl/audio/mixer.h WlMixPullFn); `user` is a
// WlSynth.
void wl_syn_pull(void *user, int16_t *stereo, uint32_t frames, bool *ended);

bool wl_syn_done(const WlSynth *s);
uint32_t wl_syn_voices_active(const WlSynth *s);
uint32_t wl_syn_tracks_active(const WlSynth *s);
// Any thread: while paused the output is silence and nothing advances.
void wl_syn_set_paused(WlSynth *s, bool paused);
// Audio thread (or before the first render): variables 0..31 (16..31 are shared). Out of range: 0 /
// no-op.
int32_t wl_syn_var_get(const WlSynth *s, uint32_t idx);
void wl_syn_var_set(WlSynth *s, uint32_t idx, int32_t value);
void wl_syn_set_trace(WlSynth *s, WlSynTraceFn fn, void *user);
const WlSynStats *wl_syn_stats(const WlSynth *s);

// Q16 playback step (wave samples per output sample) for a wave rate and a pitch offset in 1/64
// semitones, clamped to 1/64 .. 16. Exposed for tests.
uint32_t wl_syn_step(uint32_t rate, int32_t pitch64);
// Gain (Q16, 65536 = 1.0) for a level in 1/128 of a tenth of a decibel (<= 0). Exposed for tests.
uint32_t wl_syn_level_gain(int32_t level_q7);
// The tenth-of-a-decibel level of a 0..127 volume or velocity (-723 .. 0).
int32_t wl_syn_volume_db(uint32_t v);
