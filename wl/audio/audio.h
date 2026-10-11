// SPDX-License-Identifier: MIT
// Audio engine: mixer + buses, PWST stream player, PWSF sample player and SSEQ sequence voices,
// over the platform output (AESND/ASND on the console). See wl/audio/README.md.
//
// Threads. Everything below is called from the MAIN thread only (it is the single producer of the
// command ring). The mixer runs on the audio thread; commands reach it through a lock-free SPSC
// ring (never a mutex: the audio thread is the highest priority). Voice handles are assigned on the
// main thread, so a play call returns at once. Streams read their data through the IO thread
// (wl/platform/fs.h) into ring buffers; wl_audio_pump (once per frame) collects the
// completions and keeps the rings full.
#pragma once

#include "wl/audio/fmt.h"
#include "wl/audio/mixer.h"
#include "wl/audio/synth.h"
#include "wl/platform/fs.h"
#include "wl/base/base.h"

// 0 = no voice. Ids are never reused within a run.
typedef uint32_t WlAudioHandle;

typedef struct {
    uint8_t bus;       // WlBus
    uint8_t priority;  // higher wins a full mixer; default 64
    int8_t pan;        // -127..127
    uint16_t gain;     // Q8, 256 = 1.0
    int16_t pitch_cents;
    uint32_t fade_in_ms;
} WlAudioPlay;

// bus, priority 64, centre, unity, no pitch, no fade.
WlAudioPlay wl_audio_play_defaults(WlBus bus);

typedef enum {
    WL_AUDIO_GONE = 0,   // finished, stopped, or never existed
    WL_AUDIO_BUFFERING,  // stream: reading the header / filling the first second
    WL_AUDIO_PLAYING,    // also: the start command is queued
    WL_AUDIO_FAILED      // stream: IO or format error; the voice is gone
} WlAudioState;

// ---- engine ----------------------------------------------------------------------------------

// Needs wl_mem_init. Allocates the mixer (WL_TAG_AUDIO) and starts the platform output.
WlResult wl_audio_init(void);
// Stops every voice, waits for outstanding stream reads (needs the IO thread alive), stops the
// output and frees everything.
void wl_audio_shutdown(void);
bool wl_audio_ready(void);

// Collects stream read completions from wl_fs_poll (`other` receives the ones that are not
// audio's, if the caller shares the queue; NULL drops them), advances every stream's state
// machine, submits the next reads, frees finished streams. Call once per frame.
typedef void (*WlAudioOtherDoneFn)(const WlFsDone *done, void *user);
void wl_audio_pump(WlAudioOtherDoneFn other, void *user);

// For a caller that polls wl_fs_poll itself (e.g. an asset loader that shares the IO queue): offers it one
// completion; true when it was a stream read of this engine. Then call wl_audio_service once per frame instead of wl_audio_pump.
bool wl_audio_handle_done(const WlFsDone *done);
// wl_audio_pump without the polling: advances the streams, submits the next reads, frees finished sequence players.
void wl_audio_service(void);
// No stream is playing, loading or closing (shutdown order for a caller that polls the file queue itself: stop the streams, service until this is true,
// then wl_audio_shutdown).
bool wl_audio_streams_idle(void);

// Waits (up to about 200 ms) until the audio thread has processed every command issued so far.
void wl_audio_sync(void);

// ---- buses -----------------------------------------------------------------------------------

void wl_audio_bus_volume(WlBus bus, uint16_t gain_q8);
void wl_audio_master_volume(uint16_t gain_q8);
// Multiplies the music bus (256 = no ducking). Use it for jingles or dialogue; a
// speech-bus voice ducks music by 6 dB on its own (set with wl_audio_speech_duck).
void wl_audio_duck_music(uint16_t factor_q8);
void wl_audio_speech_duck(uint16_t factor_q8);

// ---- voices ----------------------------------------------------------------------------------

void wl_audio_stop(WlAudioHandle h, uint32_t fade_ms);
void wl_audio_stop_all(void);
// Live changes; pitch is relative to the source rate. Streams and samples alike.
void wl_audio_set(WlAudioHandle h, uint16_t gain_q8, int8_t pan, int16_t pitch_cents);
WlAudioState wl_audio_state(WlAudioHandle h);

// ---- samples (PWSF) --------------------------------------------------------------------------

typedef struct WlAudioSfx WlAudioSfx;

// Decodes a PWSF payload (the whole entry, uncompressed) to PCM16 in WL_TAG_AUDIO_BANK. The
// payload is not kept.
WlResult wl_audio_sfx_load(const uint8_t *pwsf, size_t size, WlAudioSfx **out);
// Stops its voices, waits for the audio thread, frees the memory.
void wl_audio_sfx_free(WlAudioSfx *sfx);
const WlSfxInfo *wl_audio_sfx_info(const WlAudioSfx *sfx);
uint32_t wl_audio_sfx_bytes(const WlAudioSfx *sfx);
// The sample's own bus, priority, volume, pan and pitch, as a starting point for overrides.
WlAudioPlay wl_audio_sfx_params(const WlAudioSfx *sfx);
// `play` NULL = wl_audio_sfx_params; otherwise it replaces them (pitch is then relative to the
// sample's rate, not added to the sample's own pitch). 0 when the command ring is full.
WlAudioHandle wl_audio_sfx_play(const WlAudioSfx *sfx, const WlAudioPlay *play);

// ---- streams (PWST) --------------------------------------------------------------------------

// Where a PWST payload lives: a byte range of a file (a whole .pwst file, or an uncompressed entry
// of an archive: offset = entry offset in the file).
typedef struct {
    const char *path; // "sd:/..." or "usb:/..."
    uint64_t offset;
    uint32_t size;
} WlAudioSpan;

#define WL_AUDIO_MAX_STREAMS 3u
#define WL_AUDIO_STREAM_BLOCK 32768u // bytes per channel per read
#define WL_AUDIO_STREAM_BLOCKS 3u    // blocks per channel in the ring

// Starts streaming; playback begins when about one second is buffered (state BUFFERING until
// then). Loops forever when the stream has a loop. WL_ERR_LIMIT when WL_AUDIO_MAX_STREAMS are
// busy, WL_ERR_NOMEM when the ring memory is unavailable. Format errors surface as
// WL_AUDIO_FAILED once the header has been read. Stop with wl_audio_stop (fade_ms for crossfades).
WlResult wl_audio_stream_play(const WlAudioSpan *span, const WlAudioPlay *play, WlAudioHandle *out);

// ---- DS sequences (SSEQ synth, synth.h) ------------------------------------------------------

// The files of one song, as stored (little-endian SDAT sub-files). The memory must stay valid until
// the sequence has stopped (wl_audio_state returns WL_AUDIO_GONE).
typedef struct {
    const uint8_t *seq; // SSEQ
    size_t seq_size;
    const uint8_t *bank; // SBNK
    size_t bank_size;
    const uint8_t *waves[4]; // the bank's wave archives (SWAR), slot order; NULL = none
    size_t wave_size[4];
} WlAudioSong;

#define WL_AUDIO_MAX_SEQS 6u // music + the one fading out + a jingle + effects

// Plays a sequence on its own synth (about 10 KiB in WL_TAG_AUDIO) as one mixer voice of
// `play->bus` (default: the music bus). `par` NULL = wl_syn_params_default. The voice ends when
// every track has ended and the notes have died (looping music never does); stop it with
// wl_audio_stop (fade_ms). WL_ERR_FORMAT when a file does not parse, WL_ERR_LIMIT when
// WL_AUDIO_MAX_SEQS are busy, WL_ERR_AGAIN when the command ring is full.
WlResult wl_audio_seq_play(const WlAudioSong *song, const WlSynParams *par, const WlAudioPlay *play,
                           WlAudioHandle *out);
// Silences and freezes (or resumes) a sequence: a jingle can pause the music with it. The output stops at once: ramp the voice's
// gain to 0 (wl_audio_set) a few blocks before pausing and back up after resuming to avoid a click (music_engine users do).
void wl_audio_seq_pause(WlAudioHandle h, bool paused);
// The same for any music voice, a stream included (a paused stream keeps its position and its buffers). False when the handle is
// not a running sequence or stream.
bool wl_audio_pause(WlAudioHandle h, bool paused);
// Copies the synth's counters (diagnostics; they may be a block old). False for an unknown handle.
bool wl_audio_seq_stats(WlAudioHandle h, WlSynStats *out);

// ---- tap (tests) -----------------------------------------------------------------------------

// Called on the audio thread with every mixed block (after the mix, before the block goes to the
// output): for capturing output in tests. Must not block or allocate. NULL removes it; the call
// returns only once the audio thread is certain not to be inside the old tap.
typedef void (*WlAudioTapFn)(void *user, const int16_t *stereo, uint32_t frames);
void wl_audio_set_tap(WlAudioTapFn fn, void *user);

// ---- statistics ------------------------------------------------------------------------------

typedef struct {
    WlMixStats mix;          // as of the last block
    uint32_t underruns;      // output blocks the hardware needed and the mixer had not finished
    uint32_t blocks_filled;
    uint32_t fill_us_max;    // longest block mix, microseconds
    uint32_t fill_us_mean;
    uint32_t cmd_drops;      // commands lost because the ring was full
    uint32_t starved_frames; // stream frames replaced by silence because data had not arrived
    uint32_t stream_reads;   // completed stream reads
    uint32_t stream_errors;
    uint32_t streams_active;
    uint32_t backend;        // WlPlatformAudioStats.backend
} WlAudioStats;
void wl_audio_stats(WlAudioStats *out);
// One INFO line with the above (for the log and self-tests).
void wl_audio_log_stats(const char *label);
