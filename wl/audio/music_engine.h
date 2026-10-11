// SPDX-License-Identifier: MIT
// Music track engine over the audio engine: plays tracks as SSEQ synth voices (wl_audio_seq_play)
// or PWST streams (wl_audio_stream_play), keeps what it loaded for a while (and effects and jingles
// for good), and frees a track's memory only after its voice has left the mixer. Where the bytes
// come from is a WlMusicLoader supplied by the application (an archive reader, files on SD, or
// songs in memory for tests).
//
// It exposes itself as a WlMusicBackend: a small table of functions that a music director (the
// application's logic for which track plays when: crossfades, jingles that pause the music, ducks)
// drives once per frame on the main thread. The director is not part of WiiLibs.
//
//   WlMusicEngine eng;  wl_music_engine_init(&eng, &loader);
//   WlMusicBackend be;  wl_music_engine_backend(&eng, &be);   // be.prepare / be.play / ... each frame: be.tick
//   shutdown: wl_music_engine_shutdown(&eng); then wl_audio_shutdown().
#pragma once

#include "wl/audio/audio.h"

// A track, jingle or effect, chosen by the application (opaque to the engine). 0 = none.
typedef uint64_t WlMusicId;
#define WL_MUSIC_NONE 0u

// How a track is used: BGM tracks are cached and evicted when unused; the others are pinned once
// loaded. The role also picks the bus (SFX: effects, UI: ui, others: music) and the priority
// (jingles above everything).
typedef enum { WL_MUSIC_ROLE_BGM = 0, WL_MUSIC_ROLE_JINGLE, WL_MUSIC_ROLE_SFX, WL_MUSIC_ROLE_UI } WlMusicRole;

// What a director needs from the engine. Every call is on the main thread.
typedef struct WlMusicBackend {
    // Makes `id` ready to play (loads it) for `role`. WL_OK: ready. WL_ERR_AGAIN: loading, ask again. WL_ERR_LIMIT: busy, ask
    // later. Anything else: this id cannot be played (not found, broken); asking again will not help.
    WlResult (*prepare)(void *user, WlMusicId id, WlMusicRole role);
    // Starts a ready track at `gain_q8` with a fade in. WL_ERR_AGAIN / WL_ERR_LIMIT: no voice free now, retry. *handle is
    // non-zero on success.
    WlResult (*play)(void *user, WlMusicId id, WlMusicRole role, uint32_t fade_in_ms, uint16_t gain_q8, uint32_t *handle);
    void (*stop)(void *user, uint32_t handle, uint32_t fade_ms); // fades out and frees; the handle is dead afterwards
    // Freezes (true) or resumes a track at its position. false: it cannot (the director then only mutes it).
    bool (*pause)(void *user, uint32_t handle, bool paused);
    void (*set_gain)(void *user, uint32_t handle, uint16_t gain_q8); // ramped by the mixer, no zipper
    bool (*alive)(void *user, uint32_t handle);                       // still playing or loading
    void (*duck)(void *user, uint16_t factor_q8);                    // music bus factor (256 = none)
    void (*tick)(void *user, uint32_t dt_ms);                         // collect load completions, free what is done
    void *user;
} WlMusicBackend;

// One loaded track: the song's files (SSEQ/SBNK/SWAR) or the stream's place in a file.
typedef struct {
    bool is_stream;
    WlAudioSong song;
    WlAudioSpan span;
} WlMusicTrack;

typedef struct WlMusicLoader {
    // Starts loading `id`. WL_OK: loading (or loaded). WL_ERR_LIMIT: no room now, ask again later. Anything else: it cannot be loaded.
    WlResult (*request)(void *user, WlMusicId id);
    // WL_OK: loaded, `out` is valid until release(). WL_ERR_AGAIN: still loading. Anything else: failed (release() still follows).
    WlResult (*poll)(void *user, WlMusicId id, WlMusicTrack *out);
    // The engine is done with `id`: no voice reads it any more. Free it or keep it cached.
    void (*release)(void *user, WlMusicId id);
    // Optional, once per tick: collect completions.
    void (*pump)(void *user);
    void *user;
} WlMusicLoader;

#define WL_MUSIC_ENGINE_ENTRIES 24u
#define WL_MUSIC_ENGINE_VOICES 16u
#define WL_MUSIC_ENGINE_KEEP 3u // unreferenced background tracks kept loaded (returning to a recent track costs no load)

typedef struct {
    WlMusicId id;
    uint8_t state; // 0 free, 1 loading, 2 ready, 3 failed
    bool pinned;   // an effect or a jingle: kept for good
    uint16_t refs; // voices playing it
    uint32_t used; // last use (a counter)
    WlMusicTrack track;
} WlMusicEntry;

typedef struct {
    uint32_t handle; // WlAudioHandle, 0 = free slot
    uint16_t entry;
} WlMusicVoice;

typedef struct {
    uint32_t loads, load_failures, evictions, plays, play_failures;
} WlMusicEngineStats;

typedef struct WlMusicEngine {
    WlMusicLoader loader;
    WlMusicEntry entry[WL_MUSIC_ENGINE_ENTRIES];
    WlMusicVoice voice[WL_MUSIC_ENGINE_VOICES];
    uint32_t clock;
    WlMusicEngineStats stats;
} WlMusicEngine;

void wl_music_engine_init(WlMusicEngine *e, const WlMusicLoader *loader);
// Stops this engine's voices at once, waits (up to about 500 ms; the audio thread must be running) until they are gone and releases
// every entry.
void wl_music_engine_shutdown(WlMusicEngine *e);
// Fills `out` (also returned).
const WlMusicBackend *wl_music_engine_backend(WlMusicEngine *e, WlMusicBackend *out);
// Entries loaded or loading, voices in use.
void wl_music_engine_usage(const WlMusicEngine *e, uint32_t *entries, uint32_t *voices);
