// SPDX-License-Identifier: MIT
// Audio engine: command ring, the fill function the platform calls on the audio thread, PWSF
// samples and the public API. Streams are in stream.c.
#include <string.h>

#include "wl/audio/adpcm.h"
#include "wl/audio/audio_internal.h"
#include "wl/platform/mem.h"
#include "wl/base/endian.h"

#define CMD_RING_CAPACITY 256u
#define CMDS_PER_BLOCK 96u
#define SYNC_TIMEOUT_MS 200u

static struct {
    bool ready;
    WlMixer *mixer;
    WlAudioCmd *cmd_store;
    WlSpscRing ring;
    // Audio thread -> main: published after each block.
    atomic_uint slot_id[WL_MIX_MAX_VOICES];
    atomic_uint cmds_done;
    atomic_uint st_blocks, st_clipped, st_active, st_peak, st_steals, st_drops, st_started;
    // Main thread only.
    uint32_t next_id;
    uint32_t cmds_issued;
    atomic_uint cmd_drops;
    _Atomic(WlAudioTapFn) tap;
    void *tap_user;
    struct SeqSlot *seq_slot[WL_AUDIO_MAX_SEQS]; // SSEQ players (main thread)
} g;

typedef struct SeqSlot {
    uint32_t handle;
    WlSynSeq seq;
    WlSynBank bank;
    WlSynth syn;
} SeqSlot;

WlAudioPlay wl_audio_play_defaults(WlBus bus) {
    WlAudioPlay p;
    memset(&p, 0, sizeof p);
    p.bus = (uint8_t)bus;
    p.priority = 64;
    p.gain = 256;
    return p;
}

// ---------------------------------------------------------------------------------------------
// Audio thread side
// ---------------------------------------------------------------------------------------------

static void apply(const WlAudioCmd *c) {
    WlMixer *m = g.mixer;
    switch (c->op) {
    case WL_ACMD_PLAY_SAMPLE:
    case WL_ACMD_PLAY_SYNTH:
    case WL_ACMD_PLAY_STREAM: {
        WlMixVoiceDesc d;
        memset(&d, 0, sizeof d);
        d.id = c->id;
        d.bus = c->bus;
        d.priority = c->priority;
        d.gain = c->gain;
        d.pan = c->pan;
        d.rate = c->rate;
        d.pitch_cents = c->pitch;
        d.fade_in_frames = c->fade_frames;
        if (c->op == WL_ACMD_PLAY_SAMPLE) {
            d.pcm = c->ptr;
            d.frames = c->frames;
            d.stereo = (c->flags & WL_ACMD_FLAG_STEREO) != 0;
            d.loop = (c->flags & WL_ACMD_FLAG_LOOP) != 0;
            d.loop_start = c->loop_start;
            d.loop_end = c->loop_end;
        } else if (c->op == WL_ACMD_PLAY_SYNTH) {
            d.pull = wl_syn_pull;
            d.pull_user = (void *)(uintptr_t)c->ptr;
        } else {
            d.pull = wl_stream_pull;
            d.pull_user = (void *)(uintptr_t)c->ptr;
        }
        (void)wl_mixer_voice_start(m, &d);
        break;
    }
    case WL_ACMD_STOP:
        wl_mixer_voice_stop(m, c->id, c->fade_frames);
        break;
    case WL_ACMD_SET:
        wl_mixer_voice_set(m, c->id, c->gain, c->pan, c->pitch);
        break;
    case WL_ACMD_BUS:
        wl_mixer_bus_set(m, (WlBus)c->bus, c->gain);
        break;
    case WL_ACMD_MASTER:
        wl_mixer_master_set(m, c->gain);
        break;
    case WL_ACMD_DUCK:
        wl_mixer_duck_set(m, c->gain);
        break;
    case WL_ACMD_SPEECH_DUCK:
        m->speech_duck = c->gain > 256 ? 256 : c->gain;
        break;
    case WL_ACMD_STOP_ALL:
        wl_mixer_stop_all(m);
        break;
    case WL_ACMD_STOP_PCM:
        wl_mixer_stop_pcm(m, c->ptr);
        break;
    default:
        break;
    }
}

// Applies queued commands (bounded per block so a flood cannot starve the output) and returns the
// sequence number of the last one processed.
static uint32_t process_commands(void) {
    WlAudioCmd c;
    uint32_t n = 0;
    uint32_t last_seq = atomic_load_explicit(&g.cmds_done, memory_order_relaxed);
    while (n < CMDS_PER_BLOCK && wl_ring_pop(&g.ring, &c)) {
        apply(&c);
        last_seq = c.seq;
        n++;
    }
    return last_seq;
}

// Publishes the slots first, then the sequence number: a reader that sees `cmds_done` >= s and then
// scans the slots sees a state at least as new as command s.
static void publish(uint32_t last_seq) {
    for (uint32_t i = 0; i < WL_MIX_MAX_VOICES; i++) {
        atomic_store_explicit(&g.slot_id[i], g.mixer->voices[i].active ? g.mixer->voices[i].id : 0u,
                              memory_order_relaxed);
    }
    atomic_store_explicit(&g.cmds_done, last_seq, memory_order_release);
}

static void audio_fill(void *user, int16_t *out, uint32_t frames) {
    (void)user;
    const uint32_t seq = process_commands();
    wl_mixer_mix(g.mixer, out, frames);
    const WlMixStats *s = &g.mixer->stats;
    atomic_store_explicit(&g.st_blocks, s->blocks, memory_order_relaxed);
    atomic_store_explicit(&g.st_clipped, s->clipped, memory_order_relaxed);
    atomic_store_explicit(&g.st_active, s->active, memory_order_relaxed);
    atomic_store_explicit(&g.st_peak, s->peak, memory_order_relaxed);
    atomic_store_explicit(&g.st_steals, s->steals, memory_order_relaxed);
    atomic_store_explicit(&g.st_drops, s->drops, memory_order_relaxed);
    atomic_store_explicit(&g.st_started, s->voices_started, memory_order_relaxed);
    WlAudioTapFn tap = atomic_load_explicit(&g.tap, memory_order_acquire);
    if (tap != NULL) {
        tap(g.tap_user, out, frames);
    }
    publish(seq);
}

// ---------------------------------------------------------------------------------------------
// Main thread side
// ---------------------------------------------------------------------------------------------

bool wl_audio_ready(void) {
    return g.ready;
}

WlResult wl_audio_init(void) {
    if (g.ready) {
        return WL_ERR_STATE;
    }
    if (!wl_mem_ready()) {
        return WL_ERR_STATE;
    }
    memset(&g, 0, sizeof g);
    g.mixer = WL_MEM_ALLOC(WL_TAG_AUDIO, sizeof(WlMixer));
    g.cmd_store = WL_MEM_ALLOC(WL_TAG_AUDIO, CMD_RING_CAPACITY * sizeof(WlAudioCmd));
    if (g.mixer == NULL || g.cmd_store == NULL ||
        wl_ring_init(&g.ring, g.cmd_store, (uint32_t)sizeof(WlAudioCmd), CMD_RING_CAPACITY) != WL_OK) {
        wl_mem_free(g.mixer);
        wl_mem_free(g.cmd_store);
        memset(&g, 0, sizeof g);
        return WL_ERR_NOMEM;
    }
    wl_mixer_init(g.mixer);
    wl_stream_reset();
    g.next_id = 1;
    WlResult r = wl_platform_audio_start(WL_MIX_RATE, WL_MIX_BLOCK, audio_fill, NULL);
    if (r != WL_OK) {
        wl_mem_free(g.mixer);
        wl_mem_free(g.cmd_store);
        memset(&g, 0, sizeof g);
        return r;
    }
    g.ready = true;
    return WL_OK;
}

void wl_audio_shutdown(void) {
    if (!g.ready) {
        return;
    }
    wl_audio_stop_all();
    wl_audio_sync();
    for (uint32_t i = 0; i < 400u && !wl_stream_idle(); i++) {
        wl_audio_pump(NULL, NULL);
        wl_platform_thread_sleep_ms(5);
    }
    wl_platform_audio_stop();
    for (uint32_t i = 0; i < WL_AUDIO_MAX_SEQS; i++) {
        wl_mem_free(g.seq_slot[i]);
    }
    wl_mem_free(g.mixer);
    wl_mem_free(g.cmd_store);
    memset(&g, 0, sizeof g);
}

bool wl_audio_push(WlAudioCmd *cmd) {
    cmd->seq = g.cmds_issued + 1u;
    if (!wl_ring_push(&g.ring, cmd)) {
        atomic_fetch_add(&g.cmd_drops, 1u);
        return false;
    }
    g.cmds_issued++;
    return true;
}

// Control commands must not be lost: wait briefly for room.
static bool push_wait(WlAudioCmd *cmd) {
    for (uint32_t i = 0; i < 50u; i++) {
        if (wl_audio_push(cmd)) {
            return true;
        }
        if (!wl_platform_audio_threaded()) {
            return false; // nobody drains on the host stub unless the test renders
        }
        wl_platform_thread_sleep_ms(2);
    }
    return false;
}

uint32_t wl_audio_cmds_done(void) {
    return atomic_load_explicit(&g.cmds_done, memory_order_acquire);
}

uint32_t wl_audio_cmds_issued(void) {
    return g.cmds_issued;
}

bool wl_audio_voice_in_mixer(uint32_t id) {
    for (uint32_t i = 0; i < WL_MIX_MAX_VOICES; i++) {
        if (id != 0 && atomic_load_explicit(&g.slot_id[i], memory_order_acquire) == id) {
            return true;
        }
    }
    return false;
}

bool wl_audio_voice_alive(uint32_t id) {
    if (id == 0 || !g.ready) {
        return false;
    }
    if (wl_audio_cmds_done() != g.cmds_issued) {
        return true; // commands in flight: unknown, assume it is (about to be) playing
    }
    return wl_audio_voice_in_mixer(id);
}

void wl_audio_sync(void) {
    if (!g.ready) {
        return;
    }
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_NOP;
    if (!wl_platform_audio_threaded()) {
        // Host stub: the caller is the audio thread, so apply the commands in place.
        (void)wl_audio_push(&c);
        publish(process_commands());
        return;
    }
    if (!push_wait(&c)) {
        return;
    }
    for (uint32_t i = 0; i < SYNC_TIMEOUT_MS; i++) {
        if (wl_audio_cmds_done() == g.cmds_issued) {
            return;
        }
        wl_platform_thread_sleep_ms(1);
    }
}

static uint32_t ms_to_frames(uint32_t ms) {
    return ms * (WL_MIX_RATE / 1000u);
}

void wl_audio_bus_volume(WlBus bus, uint16_t gain_q8) {
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_BUS;
    c.bus = (uint8_t)bus;
    c.gain = gain_q8;
    (void)push_wait(&c);
}

void wl_audio_master_volume(uint16_t gain_q8) {
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_MASTER;
    c.gain = gain_q8;
    (void)push_wait(&c);
}

void wl_audio_duck_music(uint16_t factor_q8) {
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_DUCK;
    c.gain = factor_q8;
    (void)push_wait(&c);
}

void wl_audio_speech_duck(uint16_t factor_q8) {
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_SPEECH_DUCK;
    c.gain = factor_q8;
    (void)push_wait(&c);
}

void wl_audio_stop(WlAudioHandle h, uint32_t fade_ms) {
    if (h == 0 || !g.ready) {
        return;
    }
    if (wl_stream_owns(h)) {
        wl_stream_stop(h, fade_ms);
        return;
    }
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_STOP;
    c.id = h;
    c.fade_frames = ms_to_frames(fade_ms);
    (void)push_wait(&c);
}

void wl_audio_stop_all(void) {
    if (!g.ready) {
        return;
    }
    wl_stream_stop_all();
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_STOP_ALL;
    (void)push_wait(&c);
}

void wl_audio_set(WlAudioHandle h, uint16_t gain_q8, int8_t pan, int16_t pitch_cents) {
    if (h == 0 || !g.ready) {
        return;
    }
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_SET;
    c.id = h;
    c.gain = gain_q8;
    c.pan = pan;
    c.pitch = pitch_cents;
    (void)push_wait(&c);
}

WlAudioState wl_audio_state(WlAudioHandle h) {
    if (h == 0 || !g.ready) {
        return WL_AUDIO_GONE;
    }
    if (wl_stream_owns(h)) {
        return wl_stream_state(h);
    }
    return wl_audio_voice_alive(h) ? WL_AUDIO_PLAYING : WL_AUDIO_GONE;
}

bool wl_audio_handle_done(const WlFsDone *done) {
    return g.ready && wl_stream_handle_done(done);
}

void wl_audio_service(void) {
    if (!g.ready) {
        return;
    }
    wl_stream_pump();
    // Sequence players whose voice has left the mixer.
    for (uint32_t i = 0; i < WL_AUDIO_MAX_SEQS; i++) {
        if (g.seq_slot[i] != NULL && !wl_audio_voice_alive(g.seq_slot[i]->handle)) {
            wl_mem_free(g.seq_slot[i]);
            g.seq_slot[i] = NULL;
        }
    }
}

bool wl_audio_streams_idle(void) {
    return !g.ready || wl_stream_idle();
}

void wl_audio_pump(WlAudioOtherDoneFn other, void *user) {
    if (!g.ready) {
        return;
    }
    WlFsDone d;
    while (wl_fs_poll(&d)) {
        if (!wl_stream_handle_done(&d) && other != NULL) {
            other(&d, user);
        }
    }
    wl_audio_service();
}

void wl_audio_set_tap(WlAudioTapFn fn, void *user) {
    if (!g.ready) {
        return;
    }
    atomic_store_explicit(&g.tap, NULL, memory_order_release);
    wl_audio_sync(); // the audio thread has finished any block that was using the old tap
    g.tap_user = user;
    atomic_store_explicit(&g.tap, fn, memory_order_release);
}

void wl_audio_stats(WlAudioStats *out) {
    memset(out, 0, sizeof *out);
    if (!g.ready) {
        return;
    }
    out->mix.blocks = atomic_load_explicit(&g.st_blocks, memory_order_relaxed);
    out->mix.clipped = atomic_load_explicit(&g.st_clipped, memory_order_relaxed);
    out->mix.active = atomic_load_explicit(&g.st_active, memory_order_relaxed);
    out->mix.peak = atomic_load_explicit(&g.st_peak, memory_order_relaxed);
    out->mix.steals = atomic_load_explicit(&g.st_steals, memory_order_relaxed);
    out->mix.drops = atomic_load_explicit(&g.st_drops, memory_order_relaxed);
    out->mix.voices_started = atomic_load_explicit(&g.st_started, memory_order_relaxed);
    WlPlatformAudioStats p;
    wl_platform_audio_stats(&p);
    out->underruns = p.underruns;
    out->blocks_filled = p.blocks_filled;
    out->fill_us_max = p.fill_us_max;
    out->fill_us_mean = p.blocks_filled != 0 ? p.fill_us_total / p.blocks_filled : 0;
    out->backend = p.backend;
    out->cmd_drops = atomic_load(&g.cmd_drops);
    wl_stream_stats(&out->starved_frames, &out->stream_reads, &out->stream_errors, &out->streams_active);
}

void wl_audio_log_stats(const char *label) {
    WlAudioStats s;
    wl_audio_stats(&s);
    WL_LOG_I("AUDIO",
             "%s: backend=%u blocks=%u underruns=%u fill_us max=%u mean=%u voices=%u started=%u "
             "steals=%u drops=%u clipped=%u starved=%u stream_reads=%u errors=%u cmd_drops=%u",
             label, s.backend, s.blocks_filled, s.underruns, s.fill_us_max, s.fill_us_mean,
             s.mix.active, s.mix.voices_started, s.mix.steals, s.mix.drops, s.mix.clipped,
             s.starved_frames, s.stream_reads, s.stream_errors, s.cmd_drops);
}

// ---------------------------------------------------------------------------------------------
// Samples (PWSF)
// ---------------------------------------------------------------------------------------------

struct WlAudioSfx {
    WlSfxInfo info;
    int16_t *pcm;
    uint32_t frames;
    uint32_t bytes;
};

WlResult wl_audio_sfx_load(const uint8_t *pwsf, size_t size, WlAudioSfx **out) {
    WlSfxInfo info;
    WlResult r = wl_pwsf_parse(pwsf, size, &info);
    if (r != WL_OK) {
        return r;
    }
    WlAudioSfx *s = wl_mem_alloc_at(WL_TAG_AUDIO_BANK, sizeof *s, WL_ARENA_ALIGN, __FILE__, __LINE__);
    const uint32_t padded = (info.samples + 13u) / 14u * 14u; // whole ADPCM frames
    int16_t *pcm = wl_mem_alloc_at(WL_TAG_AUDIO_BANK, (size_t)padded * 2u, WL_ARENA_ALIGN, __FILE__, __LINE__);
    if (s == NULL || pcm == NULL) {
        wl_mem_free(s);
        wl_mem_free(pcm);
        return WL_ERR_NOMEM;
    }
    const uint8_t *data = pwsf + info.data_off;
    if (info.codec == 0) {
        WlDspState st;
        memcpy(st.coef, info.ch.coef, sizeof st.coef);
        st.h1 = info.ch.init_h1;
        st.h2 = info.ch.init_h2;
        wl_dsp_decode(&st, data, padded / 14u, pcm);
    } else {
        for (uint32_t i = 0; i < info.samples; i++) {
            pcm[i] = (int16_t)wl_rd_be16(data + 2u * i);
        }
    }
    s->info = info;
    s->pcm = pcm;
    s->frames = info.samples;
    s->bytes = padded * 2u;
    *out = s;
    return WL_OK;
}

void wl_audio_sfx_free(WlAudioSfx *sfx) {
    if (sfx == NULL) {
        return;
    }
    if (g.ready) {
        WlAudioCmd c;
        memset(&c, 0, sizeof c);
        c.op = WL_ACMD_STOP_PCM;
        c.ptr = sfx->pcm;
        (void)push_wait(&c);
        wl_audio_sync(); // after this the mixer holds no reference
    }
    wl_mem_free(sfx->pcm);
    wl_mem_free(sfx);
}

const WlSfxInfo *wl_audio_sfx_info(const WlAudioSfx *sfx) {
    return &sfx->info;
}

uint32_t wl_audio_sfx_bytes(const WlAudioSfx *sfx) {
    return sfx->bytes;
}

WlAudioPlay wl_audio_sfx_params(const WlAudioSfx *sfx) {
    WlAudioPlay p;
    memset(&p, 0, sizeof p);
    p.bus = sfx->info.bus;
    p.priority = sfx->info.priority != 0 ? sfx->info.priority : 64;
    p.gain = sfx->info.volume;
    p.pan = sfx->info.pan;
    p.pitch_cents = sfx->info.pitch_cents;
    return p;
}

WlAudioHandle wl_audio_sfx_play(const WlAudioSfx *sfx, const WlAudioPlay *play) {
    if (!g.ready || sfx == NULL) {
        return 0;
    }
    const WlAudioPlay p = play != NULL ? *play : wl_audio_sfx_params(sfx);
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_PLAY_SAMPLE;
    c.id = g.next_id;
    c.ptr = sfx->pcm;
    c.rate = sfx->info.rate;
    c.pitch = p.pitch_cents;
    c.gain = p.gain;
    c.pan = p.pan;
    c.bus = p.bus;
    c.priority = p.priority != 0 ? p.priority : 64;
    c.fade_frames = ms_to_frames(p.fade_in_ms);
    c.frames = sfx->frames;
    if (sfx->info.flags & 1u) {
        c.flags |= WL_ACMD_FLAG_LOOP;
        c.loop_start = sfx->info.loop_start;
        c.loop_end = sfx->info.loop_end + 1u;
    } else if (sfx->info.play_ms != 0) {
        const uint64_t lim = (uint64_t)sfx->info.play_ms * sfx->info.rate / 1000u;
        if (lim > 0 && lim < c.frames) {
            c.frames = (uint32_t)lim;
        }
    }
    if (!wl_audio_push(&c)) {
        return 0;
    }
    return g.next_id++;
}

WlResult wl_audio_stream_play(const WlAudioSpan *span, const WlAudioPlay *play, WlAudioHandle *out) {
    if (!g.ready) {
        return WL_ERR_STATE;
    }
    if (span == NULL || play == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    WlResult r = wl_stream_start(span, play, g.next_id);
    if (r != WL_OK) {
        return r;
    }
    *out = g.next_id++;
    return WL_OK;
}

// ---------------------------------------------------------------------------------------------
// DS sequences
// ---------------------------------------------------------------------------------------------

static SeqSlot *seq_find(WlAudioHandle h) {
    for (uint32_t i = 0; h != 0 && i < WL_AUDIO_MAX_SEQS; i++) {
        if (g.seq_slot[i] != NULL && g.seq_slot[i]->handle == h) {
            return g.seq_slot[i];
        }
    }
    return NULL;
}

WlResult wl_audio_seq_play(const WlAudioSong *song, const WlSynParams *par, const WlAudioPlay *play,
                           WlAudioHandle *out) {
    if (!g.ready) {
        return WL_ERR_STATE;
    }
    if (song == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    // Free slots whose voice is gone first, so a busy ring of finished songs does not refuse a new one.
    for (uint32_t i = 0; i < WL_AUDIO_MAX_SEQS; i++) {
        if (g.seq_slot[i] != NULL && !wl_audio_voice_alive(g.seq_slot[i]->handle)) {
            wl_mem_free(g.seq_slot[i]);
            g.seq_slot[i] = NULL;
        }
    }
    uint32_t slot = 0;
    while (slot < WL_AUDIO_MAX_SEQS && g.seq_slot[slot] != NULL) {
        slot++;
    }
    if (slot == WL_AUDIO_MAX_SEQS) {
        return WL_ERR_LIMIT;
    }
    SeqSlot *sl = wl_mem_alloc_at(WL_TAG_AUDIO, sizeof *sl, WL_ARENA_ALIGN, __FILE__, __LINE__);
    if (sl == NULL) {
        return WL_ERR_NOMEM;
    }
    memset(sl, 0, sizeof *sl);
    WlResult r = wl_syn_seq_open(&sl->seq, song->seq, song->seq_size);
    if (r == WL_OK) {
        r = wl_syn_bank_open(&sl->bank, song->bank, song->bank_size);
    }
    for (uint32_t i = 0; r == WL_OK && i < 4u; i++) {
        if (song->waves[i] != NULL) {
            WlSynWaveArc wa;
            r = wl_syn_waves_open(&wa, song->waves[i], song->wave_size[i]);
            if (r == WL_OK) {
                wl_syn_bank_set_waves(&sl->bank, i, &wa);
            }
        }
    }
    if (r == WL_OK) {
        r = wl_syn_init(&sl->syn, &sl->seq, &sl->bank, par);
    }
    if (r != WL_OK) {
        wl_mem_free(sl);
        return r == WL_ERR_ARG ? WL_ERR_ARG : WL_ERR_FORMAT;
    }
    const WlAudioPlay p = play != NULL ? *play : wl_audio_play_defaults(WL_BUS_MUSIC);
    WlAudioCmd c;
    memset(&c, 0, sizeof c);
    c.op = WL_ACMD_PLAY_SYNTH;
    c.id = g.next_id;
    c.ptr = &sl->syn;
    c.gain = p.gain != 0 ? p.gain : 256;
    c.pan = p.pan;
    c.bus = p.bus;
    c.priority = p.priority != 0 ? p.priority : 64;
    c.fade_frames = ms_to_frames(p.fade_in_ms);
    if (!wl_audio_push(&c)) {
        wl_mem_free(sl);
        return WL_ERR_AGAIN;
    }
    sl->handle = g.next_id++;
    g.seq_slot[slot] = sl;
    *out = sl->handle;
    return WL_OK;
}

void wl_audio_seq_pause(WlAudioHandle h, bool paused) {
    SeqSlot *sl = seq_find(h);
    if (sl != NULL) {
        wl_syn_set_paused(&sl->syn, paused);
    }
}

bool wl_audio_pause(WlAudioHandle h, bool paused) {
    if (h == 0 || !g.ready) {
        return false;
    }
    SeqSlot *sl = seq_find(h);
    if (sl != NULL) {
        wl_syn_set_paused(&sl->syn, paused);
        return true;
    }
    return wl_stream_owns(h) && wl_stream_set_paused(h, paused);
}

bool wl_audio_seq_stats(WlAudioHandle h, WlSynStats *out) {
    const SeqSlot *sl = seq_find(h);
    if (sl == NULL || out == NULL) {
        return false;
    }
    *out = sl->syn.stats;
    return true;
}
