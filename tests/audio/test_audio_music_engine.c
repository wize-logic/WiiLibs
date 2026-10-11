// SPDX-License-Identifier: MIT
// The music engine backend (wl/audio/music_engine.c) driven call by call through its WlMusicBackend table, over a scripted loader
// and, for the voices, the real audio engine on the host stub (the test renders blocks itself): prepare (bad id, no loader, refusals,
// remembered failures, slow loads, a full table), play before the track is ready, voice limits, pinned and kept tracks, the LRU eviction
// beyond WL_MUSIC_ENGINE_KEEP, a song that ends and one that loops, pause / gain / duck / stop on live and dead handles, the stats and
// usage counters, and shutdown (idempotent, every track released). Track ids are the engine's opaque 64-bit WlMusicId; the songs are
// synthetic tones built with synth_fixture.h.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wl/audio/audio.h"
#include "wl/audio/music_engine.h"
#include "wl/platform/host/audio_host.h"
#include "wl/platform/mem.h"
#include "tests/audio/synth_fixture.h"
#include "greatest.h"

// ---- synthetic songs ------------------------------------------------------------------------------------------------

#define MF_WAVE_SAMPLES 64u // one period of the sine at 32 kHz: 500 Hz at key 60

typedef struct {
    uint8_t seq[64];
    uint8_t bank[256];
    uint8_t war[512];
    size_t seq_n, bank_n, war_n;
} MfSong;

// A sustained sine tone at `key` as a looping PCM16 wave on a one-instrument bank. `ticks` 0 = lasts for ever (a looping
// background track), else the note lasts that many ticks (96 per second at the default tempo) and releases in about 60 ms.
// false: the files did not fit.
static bool mf_song_tone(MfSong *s, uint8_t key, uint32_t ticks) {
    memset(s, 0, sizeof *s);
    // The wave: one sine period, 12,000 peak, starting at zero.
    static int16_t wave[MF_WAVE_SAMPLES];
    static bool made;
    if (!made) {
        for (uint32_t i = 0; i < MF_WAVE_SAMPLES; i++) {
            wave[i] = (int16_t)lrint(12000.0 * sin(6.283185307179586 * (double)i / (double)MF_WAVE_SAMPLES));
        }
        made = true;
    }
    uint8_t data[MF_WAVE_SAMPLES * 2u];
    for (uint32_t i = 0; i < MF_WAVE_SAMPLES; i++) {
        data[2u * i] = (uint8_t)((uint16_t)wave[i] & 0xFFu);
        data[2u * i + 1u] = (uint8_t)((uint16_t)wave[i] >> 8);
    }
    SynfixWave w;
    memset(&w, 0, sizeof w);
    w.enc = 1;
    w.loop = true;
    w.rate = 32000;
    w.loop_start_w = 0;
    w.loop_len_w = MF_WAVE_SAMPLES / 2u;
    w.data = data;
    s->war_n = synfix_swar(s->war, sizeof s->war, &w, 1);
    // Instant attack (the wave starts at zero), full sustain, a 60 ms release.
    SynfixInst in = synfix_single(1, 0, 60, 127, 127, 127, 125);
    s->bank_n = synfix_sbnk(s->bank, sizeof s->bank, &in, 1);
    uint8_t cmds[16];
    uint32_t n = 0;
    cmds[n++] = key;
    cmds[n++] = 100;
    const uint32_t len = ticks != 0 ? ticks : 0x0FFFFFFFu;
    uint8_t v[4];
    uint32_t nv = 0;
    for (int sh = 21; sh >= 0; sh -= 7) {
        if (nv != 0 || ((len >> sh) & 0x7Fu) != 0 || sh == 0) {
            v[nv++] = (uint8_t)((len >> sh) & 0x7Fu);
        }
    }
    for (uint32_t i = 0; i < nv; i++) {
        cmds[n++] = (uint8_t)(v[i] | (i + 1u < nv ? 0x80u : 0u));
    }
    cmds[n++] = 0xFF;
    s->seq_n = synfix_sseq(s->seq, sizeof s->seq, cmds, n);
    return s->war_n != 0 && s->bank_n != 0 && s->seq_n != 0;
}

static WlAudioSong mf_audio_song(const MfSong *s) {
    WlAudioSong a;
    memset(&a, 0, sizeof a);
    a.seq = s->seq;
    a.seq_size = s->seq_n;
    a.bank = s->bank;
    a.bank_size = s->bank_n;
    a.waves[0] = s->war;
    a.wave_size[0] = s->war_n;
    return a;
}

// ---- a scripted loader over the fixture's songs -------------------------------------------------------------------

#define NIDS 40u
static MfSong g_loop_song, g_short_song;

typedef struct {
    WlResult request_result; // WL_OK = serve
    WlResult poll_result;    // WL_OK = serve, else returned instead (AGAIN counts down `latency` first)
    uint32_t latency;        // pump calls before poll serves
    uint32_t requests, polls, releases, pumps;
    uint32_t released_ids[64];
    bool short_ids;          // ids >= 1000 are the ending song
    uint32_t pumped[NIDS];
    bool requested[NIDS];
} Script;

static Script g_s;

static uint32_t slot_of(WlMusicId id) {
    return (uint32_t)(id % NIDS);
}

static WlResult sc_request(void *u, WlMusicId id) {
    Script *s = u;
    s->requests++;
    if (s->request_result != WL_OK) {
        return s->request_result;
    }
    s->requested[slot_of(id)] = true;
    s->pumped[slot_of(id)] = 0;
    return WL_OK;
}

static WlResult sc_poll(void *u, WlMusicId id, WlMusicTrack *out) {
    Script *s = u;
    s->polls++;
    if (s->poll_result != WL_OK) {
        return s->poll_result;
    }
    if (s->pumped[slot_of(id)] < s->latency) {
        return WL_ERR_AGAIN;
    }
    memset(out, 0, sizeof *out);
    out->song = mf_audio_song(id >= 1000u ? &g_short_song : &g_loop_song);
    return WL_OK;
}

static void sc_release(void *u, WlMusicId id) {
    Script *s = u;
    if (s->releases < 64u) {
        s->released_ids[s->releases] = (uint32_t)id;
    }
    s->releases++;
    s->requested[slot_of(id)] = false;
}

static void sc_pump(void *u) {
    Script *s = u;
    s->pumps++;
    for (uint32_t i = 0; i < NIDS; i++) {
        s->pumped[i]++;
    }
}

static WlMusicLoader g_ld;
static WlMusicEngine g_e;
static WlMusicBackend g_be;

static void bring_up(bool with_loader, bool with_pump) {
    memset(&g_s, 0, sizeof g_s);
    memset(&g_ld, 0, sizeof g_ld);
    g_ld.request = sc_request;
    g_ld.poll = sc_poll;
    g_ld.release = sc_release;
    g_ld.pump = with_pump ? sc_pump : NULL;
    g_ld.user = &g_s;
    wl_music_engine_init(&g_e, with_loader ? &g_ld : NULL);
    (void)wl_music_engine_backend(&g_e, &g_be);
}

static int16_t g_out[512 * 64];

static void render(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; b++) {
        wl_host_audio_render(g_out + 512u * (b % 64u), 256);
    }
}

static int32_t peak_of_last(uint32_t blocks) {
    int32_t p = 0;
    for (uint32_t i = 0; i < 512u * blocks; i++) {
        const int32_t v = g_out[i] < 0 ? -g_out[i] : g_out[i];
        p = v > p ? v : p;
    }
    return p;
}

// One game frame: the engine's tick, two blocks of sound, the service that frees finished players.
static void frame(void) {
    g_be.tick(g_be.user, 16);
    render(2);
    wl_audio_pump(NULL, NULL);
}

static void frames(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        frame();
    }
}

// Prepares until ready (the loader's latency counts pumps).
static WlResult ready(WlMusicId id, WlMusicRole role) {
    WlResult r = WL_ERR_AGAIN;
    for (uint32_t i = 0; i < 20 && r == WL_ERR_AGAIN; i++) {
        r = g_be.prepare(g_be.user, id, role);
        if (r == WL_ERR_AGAIN) {
            frame();
        }
    }
    return r;
}

static uint32_t usage_entries(void) {
    uint32_t n = 0, v = 0;
    wl_music_engine_usage(&g_e, &n, &v);
    return n;
}

static uint32_t usage_voices(void) {
    uint32_t n = 0, v = 0;
    wl_music_engine_usage(&g_e, &n, &v);
    return v;
}

static void world_down(void) {
    wl_audio_stop_all(); // so the shutdown below does not wait for voices nobody renders
    render(4);
    wl_audio_pump(NULL, NULL);
    wl_music_engine_shutdown(&g_e);
    wl_audio_shutdown();
}

static bool world_up(void) {
    wl_host_audio_thread_stop();
    wl_audio_shutdown();
    (void)wl_mem_shutdown();
    if (wl_mem_init(NULL) != WL_OK || wl_audio_init() != WL_OK) {
        return false;
    }
    wl_audio_bus_volume(WL_BUS_MUSIC, 256);
    wl_audio_sync();
    return mf_song_tone(&g_loop_song, 60, 0) && mf_song_tone(&g_short_song, 60, 12);
}

// ---- tests --------------------------------------------------------------------------------------------------------

TEST backend_table_is_complete_and_usage_starts_empty(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT(g_be.prepare && g_be.play && g_be.stop && g_be.pause && g_be.set_gain && g_be.alive && g_be.duck && g_be.tick);
    ASSERT_EQ(&g_e, g_be.user);
    ASSERT_EQ(0u, usage_entries() + usage_voices());
    // NULL out pointers are allowed
    wl_music_engine_usage(&g_e, NULL, NULL);
    world_down();
    PASS();
}

TEST id_zero_is_refused_and_nothing_is_asked(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_ERR_ARG, g_be.prepare(g_be.user, 0, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(0u, g_s.requests);
    ASSERT_EQ(0u, usage_entries());
    world_down();
    PASS();
}

TEST an_engine_without_a_loader_cannot_prepare_and_remembers_it(void) {
    ASSERT(world_up());
    bring_up(false, false);
    ASSERT_EQ(WL_ERR_STATE, g_be.prepare(g_be.user, 5, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(1u, g_e.stats.load_failures);
    ASSERT_EQ(1u, usage_entries()); // the refusal is remembered...
    ASSERT_EQ(WL_ERR_NOT_FOUND, g_be.prepare(g_be.user, 5, WL_MUSIC_ROLE_BGM)); // ...so the second ask is answered from it
    ASSERT_EQ(1u, g_e.stats.load_failures);
    world_down(); // shutdown with a NULL release must not crash
    PASS();
}

TEST an_unknown_track_fails_once_and_is_not_asked_again(void) {
    ASSERT(world_up());
    bring_up(true, true);
    g_s.request_result = WL_ERR_NOT_FOUND;
    ASSERT_EQ(WL_ERR_NOT_FOUND, g_be.prepare(g_be.user, 77, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(1u, g_s.requests);
    ASSERT_EQ(1u, g_e.stats.load_failures);
    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_EQ(WL_ERR_NOT_FOUND, g_be.prepare(g_be.user, 77, WL_MUSIC_ROLE_BGM));
    }
    ASSERT_EQ(1u, g_s.requests); // an effect asked for every frame does not ask the loader every frame
    ASSERT_EQ(1u, g_e.stats.load_failures);
    // and play of it says "not ready", with no voice
    uint32_t h = 0;
    ASSERT_EQ(WL_ERR_AGAIN, g_be.play(g_be.user, 77, WL_MUSIC_ROLE_BGM, 0, 256, &h));
    ASSERT_EQ(0u, h);
    ASSERT_EQ(0u, g_e.stats.plays);
    world_down();
    ASSERT_EQ(1u, g_s.releases); // the failed entry is released with the rest
    PASS();
}

TEST a_busy_loader_is_not_remembered_and_is_asked_again(void) {
    ASSERT(world_up());
    bring_up(true, true);
    g_s.request_result = WL_ERR_LIMIT;
    ASSERT_EQ(WL_ERR_LIMIT, g_be.prepare(g_be.user, 3, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(WL_ERR_LIMIT, g_be.prepare(g_be.user, 3, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(2u, g_s.requests);
    ASSERT_EQ(0u, g_e.stats.load_failures);
    ASSERT_EQ(0u, usage_entries());
    g_s.request_result = WL_OK;
    ASSERT_EQ(WL_OK, ready(3, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(1u, g_e.stats.loads);
    world_down();
    PASS();
}

TEST a_slow_load_answers_again_until_the_pumps_have_run(void) {
    ASSERT(world_up());
    bring_up(true, true);
    g_s.latency = 3;
    ASSERT_EQ(WL_ERR_AGAIN, g_be.prepare(g_be.user, 4, WL_MUSIC_ROLE_BGM));
    uint32_t h = 0;
    ASSERT_EQ(WL_ERR_AGAIN, g_be.play(g_be.user, 4, WL_MUSIC_ROLE_BGM, 0, 256, &h)); // not ready: no voice
    ASSERT_EQ(0u, usage_voices());
    // the tick pumps the loader and collects the completion on its own: the next prepare is instant
    g_be.tick(g_be.user, 16);
    g_be.tick(g_be.user, 16);
    g_be.tick(g_be.user, 16);
    ASSERT_EQ(3u, g_s.pumps);
    ASSERT_EQ(WL_OK, g_be.prepare(g_be.user, 4, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(1u, g_e.stats.loads);
    ASSERT_EQ(1u, g_s.requests); // one request, however often it was asked
    world_down();
    PASS();
}

TEST a_load_that_fails_while_loading_is_a_failure_for_good(void) {
    ASSERT(world_up());
    bring_up(true, false); // no pump callback is allowed
    g_s.poll_result = WL_ERR_AGAIN;
    ASSERT_EQ(WL_ERR_AGAIN, g_be.prepare(g_be.user, 6, WL_MUSIC_ROLE_BGM));
    g_s.poll_result = WL_ERR_IO;
    g_be.tick(g_be.user, 16); // the tick polls loading entries
    ASSERT_EQ(1u, g_e.stats.load_failures);
    ASSERT_EQ(WL_ERR_NOT_FOUND, g_be.prepare(g_be.user, 6, WL_MUSIC_ROLE_BGM));
    // a failure seen by prepare itself is reported as the loader's own error
    ASSERT_EQ(WL_ERR_IO, g_be.prepare(g_be.user, 7, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(2u, g_e.stats.load_failures);
    world_down();
    PASS();
}

TEST play_gives_a_live_handle_and_the_track_stays_referenced(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_OK, ready(1, WL_MUSIC_ROLE_BGM));
    uint32_t h = 0;
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 1, WL_MUSIC_ROLE_BGM, 100, 256, &h));
    ASSERT(h != 0);
    ASSERT(g_be.alive(g_be.user, h));
    ASSERT_EQ(1u, usage_voices());
    ASSERT_EQ(1u, g_e.stats.plays);
    frames(40);
    ASSERT(peak_of_last(2) > 1000); // it is heard
    // many frames later it still loops, still alive, still held by its voice
    frames(400);
    ASSERT(g_be.alive(g_be.user, h));
    ASSERT_EQ(1u, usage_voices());
    ASSERT_EQ(0u, g_s.releases);
    world_down();
    PASS();
}

TEST a_second_play_of_the_same_track_shares_the_entry(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_OK, ready(1, WL_MUSIC_ROLE_SFX));
    uint32_t h1 = 0, h2 = 0;
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 1, WL_MUSIC_ROLE_SFX, 0, 256, &h1));
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 1, WL_MUSIC_ROLE_SFX, 0, 256, &h2));
    ASSERT(h1 != h2);
    ASSERT_EQ(1u, usage_entries());
    ASSERT_EQ(2u, usage_voices());
    ASSERT_EQ(1u, g_s.requests);
    g_be.stop(g_be.user, h1, 0);
    frames(10);
    ASSERT_EQ(1u, usage_voices()); // one voice left holding the entry
    ASSERT(g_be.alive(g_be.user, h2));
    world_down();
    PASS();
}

TEST a_song_that_ends_frees_its_voice_and_a_looping_one_does_not(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_OK, ready(2, WL_MUSIC_ROLE_BGM));    // loops for ever
    ASSERT_EQ(WL_OK, ready(1002, WL_MUSIC_ROLE_BGM)); // 12 ticks: about 125 ms and a release
    uint32_t hl = 0, hs = 0;
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 2, WL_MUSIC_ROLE_BGM, 0, 256, &hl));
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 1002, WL_MUSIC_ROLE_BGM, 0, 256, &hs));
    ASSERT_EQ(2u, usage_voices());
    frames(60); // ~ 1 s
    ASSERT(!g_be.alive(g_be.user, hs));
    ASSERT(g_be.alive(g_be.user, hl));
    ASSERT_EQ(1u, usage_voices()); // the tick reaped the finished one
    ASSERT_EQ(2u, usage_entries()); // both tracks are kept (under the keep count)
    // a dead handle: every call is a harmless no-op
    ASSERT(!g_be.pause(g_be.user, hs, true));
    g_be.set_gain(g_be.user, hs, 100);
    g_be.stop(g_be.user, hs, 0);
    ASSERT(!g_be.alive(g_be.user, 0));
    ASSERT(!g_be.pause(g_be.user, 0, true));
    // the ended song can be played again from its kept entry without a new request
    const uint32_t req = g_s.requests;
    ASSERT_EQ(WL_OK, g_be.prepare(g_be.user, 1002, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 1002, WL_MUSIC_ROLE_BGM, 0, 256, &hs));
    ASSERT_EQ(req, g_s.requests);
    world_down();
    PASS();
}

TEST stop_with_a_fade_ends_the_voice_and_the_track_is_kept_for_a_while(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_OK, ready(8, WL_MUSIC_ROLE_BGM));
    uint32_t h = 0;
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 8, WL_MUSIC_ROLE_BGM, 0, 256, &h));
    frames(10);
    g_be.stop(g_be.user, h, 200);
    frames(5);
    ASSERT(g_be.alive(g_be.user, h)); // still fading
    frames(40);
    ASSERT(!g_be.alive(g_be.user, h));
    ASSERT_EQ(0u, usage_voices());
    ASSERT_EQ(1u, usage_entries()); // kept: a quick return costs no load
    ASSERT_EQ(0u, g_s.releases);
    world_down();
    ASSERT_EQ(1u, g_s.releases);
    PASS();
}

TEST pause_gain_and_duck_reach_the_voice(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_OK, ready(9, WL_MUSIC_ROLE_BGM));
    uint32_t h = 0;
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 9, WL_MUSIC_ROLE_BGM, 0, 256, &h));
    frames(40);
    const int32_t full = peak_of_last(2);
    ASSERT(full > 1000);
    // gain
    g_be.set_gain(g_be.user, h, 64);
    frames(20);
    const int32_t quarter = peak_of_last(2);
    ASSERT(quarter > full / 8 && quarter < full / 2);
    g_be.set_gain(g_be.user, h, 256);
    frames(20);
    ASSERT(peak_of_last(2) > full / 2);
    // pause: silent; resume: sound again; both answered true on a live handle
    ASSERT(g_be.pause(g_be.user, h, true));
    frames(10);
    ASSERT(peak_of_last(2) < 50);
    ASSERT(g_be.alive(g_be.user, h)); // paused is not dead
    ASSERT(g_be.pause(g_be.user, h, false));
    frames(20);
    ASSERT(peak_of_last(2) > full / 2);
    // duck the music bus to nothing and back
    g_be.duck(g_be.user, 0);
    frames(20);
    ASSERT(peak_of_last(2) < 50);
    g_be.duck(g_be.user, 256);
    frames(20);
    ASSERT(peak_of_last(2) > full / 2);
    world_down();
    PASS();
}

TEST the_seventh_voice_is_refused_and_counted_and_nothing_leaks(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_OK, ready(10, WL_MUSIC_ROLE_SFX));
    uint32_t h[8] = {0};
    uint32_t ok = 0;
    WlResult last = WL_OK;
    for (uint32_t i = 0; i < 8; i++) {
        last = g_be.play(g_be.user, 10, WL_MUSIC_ROLE_SFX, 0, 256, &h[i]);
        if (last == WL_OK) {
            ok++;
        } else {
            ASSERT_EQ(0u, h[i]);
        }
    }
    ASSERT_EQ(WL_AUDIO_MAX_SEQS, ok);
    ASSERT_EQ(WL_ERR_LIMIT, last);
    ASSERT_EQ(8u - WL_AUDIO_MAX_SEQS, g_e.stats.play_failures);
    ASSERT_EQ(WL_AUDIO_MAX_SEQS, g_e.stats.plays);
    ASSERT_EQ(WL_AUDIO_MAX_SEQS, usage_voices());
    world_down();
    ASSERT_EQ(0u, usage_voices());
    PASS();
}

TEST unreferenced_background_tracks_beyond_the_keep_count_are_released_oldest_first(void) {
    ASSERT(world_up());
    bring_up(true, true);
    for (uint32_t id = 1; id <= 6; id++) {
        ASSERT_EQ(WL_OK, ready(id, WL_MUSIC_ROLE_BGM));
    }
    frames(2);
    ASSERT_EQ(WL_MUSIC_ENGINE_KEEP, usage_entries());
    ASSERT_EQ(6u - WL_MUSIC_ENGINE_KEEP, g_s.releases);
    ASSERT_EQ(6u - WL_MUSIC_ENGINE_KEEP, g_e.stats.evictions);
    // the oldest three went (1, 2, 3), the newest three are kept
    for (uint32_t i = 0; i < g_s.releases; i++) {
        ASSERT(g_s.released_ids[i] >= 1u && g_s.released_ids[i] <= 3u);
    }
    const uint32_t req = g_s.requests;
    ASSERT_EQ(WL_OK, g_be.prepare(g_be.user, 6, WL_MUSIC_ROLE_BGM)); // kept: no request
    ASSERT_EQ(req, g_s.requests);
    ASSERT_EQ(WL_OK, ready(1, WL_MUSIC_ROLE_BGM)); // an evicted one is loaded again
    ASSERT_EQ(req + 1u, g_s.requests);
    world_down();
    PASS();
}

TEST a_track_in_use_and_pinned_tracks_survive_the_eviction(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_OK, ready(1, WL_MUSIC_ROLE_BGM));
    uint32_t h = 0;
    ASSERT_EQ(WL_OK, g_be.play(g_be.user, 1, WL_MUSIC_ROLE_BGM, 0, 256, &h)); // playing: referenced
    ASSERT_EQ(WL_OK, ready(2, WL_MUSIC_ROLE_SFX));    // effect: pinned
    ASSERT_EQ(WL_OK, ready(3, WL_MUSIC_ROLE_JINGLE)); // jingle: pinned
    ASSERT_EQ(WL_OK, ready(4, WL_MUSIC_ROLE_UI));     // ui sound: pinned
    for (uint32_t id = 10; id < 20; id++) {
        ASSERT_EQ(WL_OK, ready(id, WL_MUSIC_ROLE_BGM));
    }
    frames(4);
    ASSERT_EQ(4u + WL_MUSIC_ENGINE_KEEP, usage_entries());
    for (uint32_t i = 0; i < g_s.releases; i++) {
        ASSERT(g_s.released_ids[i] >= 10u); // none of 1..4
    }
    ASSERT(g_be.alive(g_be.user, h));
    // a BGM track later played as an effect becomes pinned
    ASSERT_EQ(WL_OK, g_be.prepare(g_be.user, 19, WL_MUSIC_ROLE_SFX));
    frames(2);
    ASSERT_EQ(4u + WL_MUSIC_ENGINE_KEEP, usage_entries());
    world_down();
    PASS();
}

TEST a_full_table_of_held_tracks_says_limit_without_asking_the_loader(void) {
    ASSERT(world_up());
    bring_up(true, true);
    for (uint32_t id = 1; id <= WL_MUSIC_ENGINE_ENTRIES; id++) {
        ASSERT_EQ(WL_OK, ready(id, WL_MUSIC_ROLE_SFX)); // pinned
    }
    ASSERT_EQ(WL_MUSIC_ENGINE_ENTRIES, usage_entries());
    const uint32_t req = g_s.requests;
    ASSERT_EQ(WL_ERR_LIMIT, g_be.prepare(g_be.user, 99, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(req, g_s.requests);
    ASSERT_EQ(0u, g_e.stats.load_failures);
    world_down();
    ASSERT_EQ(WL_MUSIC_ENGINE_ENTRIES, g_s.releases);
    PASS();
}

TEST a_new_track_evicts_the_oldest_unheld_one_when_the_table_is_full(void) {
    ASSERT(world_up());
    bring_up(true, true);
    // 21 pinned, 3 background: the table is full; a 25th asks for a slot and takes the oldest background track
    for (uint32_t id = 1; id <= 21; id++) {
        ASSERT_EQ(WL_OK, ready(id, WL_MUSIC_ROLE_SFX));
    }
    for (uint32_t id = 30; id <= 32; id++) {
        ASSERT_EQ(WL_OK, ready(id, WL_MUSIC_ROLE_BGM));
    }
    ASSERT_EQ(WL_MUSIC_ENGINE_ENTRIES, usage_entries());
    ASSERT_EQ(WL_OK, ready(33, WL_MUSIC_ROLE_BGM));
    ASSERT_EQ(WL_MUSIC_ENGINE_ENTRIES, usage_entries());
    ASSERT_EQ(1u, g_s.releases);
    ASSERT_EQ(30u, g_s.released_ids[0]);
    world_down();
    PASS();
}

TEST shutdown_stops_voices_releases_everything_and_may_run_twice(void) {
    ASSERT(world_up());
    bring_up(true, true);
    uint32_t h[3] = {0};
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_EQ(WL_OK, ready(1 + i, i == 0 ? WL_MUSIC_ROLE_BGM : WL_MUSIC_ROLE_SFX));
        ASSERT_EQ(WL_OK, g_be.play(g_be.user, 1 + i, WL_MUSIC_ROLE_BGM, 0, 256, &h[i]));
    }
    frames(10);
    ASSERT_EQ(3u, usage_voices());
    // the audio thread must run for the shutdown to see the voices go: here a helper thread renders
    ASSERT_EQ(WL_OK, wl_host_audio_thread_start());
    wl_music_engine_shutdown(&g_e);
    wl_host_audio_thread_stop();
    ASSERT_EQ(0u, usage_voices() + usage_entries());
    ASSERT_EQ(3u, g_s.releases);
    for (uint32_t i = 0; i < 3; i++) {
        ASSERT(!g_be.alive(g_be.user, h[i]));
    }
    wl_music_engine_shutdown(&g_e); // nothing left to do
    ASSERT_EQ(3u, g_s.releases);
    // the engine is reusable after it
    ASSERT_EQ(WL_OK, ready(5, WL_MUSIC_ROLE_BGM));
    world_down();
    wl_audio_shutdown();
    PASS();
}

TEST init_clears_a_dirty_engine_and_copies_the_loader(void) {
    ASSERT(world_up());
    bring_up(true, true);
    ASSERT_EQ(WL_OK, ready(1, WL_MUSIC_ROLE_BGM));
    world_down();
    ASSERT(world_up());
    memset(&g_e, 0xAB, sizeof g_e);
    wl_music_engine_init(&g_e, &g_ld);
    ASSERT_EQ(0u, usage_entries() + usage_voices());
    ASSERT_EQ(0u, g_e.stats.loads + g_e.stats.plays + g_e.stats.evictions);
    memset(&g_ld, 0, sizeof g_ld); // the engine holds a copy
    ASSERT(g_e.loader.request == sc_request);
    WlMusicBackend be;
    ASSERT_EQ(&be, wl_music_engine_backend(&g_e, &be));
    ASSERT_EQ((void *)&g_e, be.user);
    wl_audio_shutdown();
    PASS();
}

SUITE(music_engine) {
    RUN_TEST(backend_table_is_complete_and_usage_starts_empty);
    RUN_TEST(id_zero_is_refused_and_nothing_is_asked);
    RUN_TEST(an_engine_without_a_loader_cannot_prepare_and_remembers_it);
    RUN_TEST(an_unknown_track_fails_once_and_is_not_asked_again);
    RUN_TEST(a_busy_loader_is_not_remembered_and_is_asked_again);
    RUN_TEST(a_slow_load_answers_again_until_the_pumps_have_run);
    RUN_TEST(a_load_that_fails_while_loading_is_a_failure_for_good);
    RUN_TEST(play_gives_a_live_handle_and_the_track_stays_referenced);
    RUN_TEST(a_second_play_of_the_same_track_shares_the_entry);
    RUN_TEST(a_song_that_ends_frees_its_voice_and_a_looping_one_does_not);
    RUN_TEST(stop_with_a_fade_ends_the_voice_and_the_track_is_kept_for_a_while);
    RUN_TEST(pause_gain_and_duck_reach_the_voice);
    RUN_TEST(the_seventh_voice_is_refused_and_counted_and_nothing_leaks);
    RUN_TEST(unreferenced_background_tracks_beyond_the_keep_count_are_released_oldest_first);
    RUN_TEST(a_track_in_use_and_pinned_tracks_survive_the_eviction);
    RUN_TEST(a_full_table_of_held_tracks_says_limit_without_asking_the_loader);
    RUN_TEST(a_new_track_evicts_the_oldest_unheld_one_when_the_table_is_full);
    RUN_TEST(shutdown_stops_voices_releases_everything_and_may_run_twice);
    RUN_TEST(init_clears_a_dirty_engine_and_copies_the_loader);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(music_engine);
    GREATEST_MAIN_END();
}
