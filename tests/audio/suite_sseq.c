// SPDX-License-Identifier: MIT
// The `sseq` suite: the SSEQ synth (wl/audio/synth*.c) against hand-written synthetic sequences,
// banks and wave archives (synth_fixture.c; nothing from game audio).
//   wlt_sseq_core   deterministic checks that run on the host, and in the big-endian (ppc-be)
//   test:
//                   tables, parsers, every command, instruments, envelopes, pitch, loops, ADPCM
//                   exactness, voice stealing, and the digest of a rendered piece (the same
//                   constant on every platform)
//   wlt_suite_sseq  wlt_sseq_core, then the mixer cost per voice, then the engine path with real
//   output
// The checks restate the format as specified (wl/audio/README.md and the community SSEQ notes); expected values come
// from the specification, not from the code under test.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wl/audio/adpcm.h"
#include "wl/audio/audio.h"
#include "wl/audio/synth.h"
#include "wl/platform/mem.h"
#include "wl/platform/platform.h"
#include "tests/audio/synth_fixture.h"
#include "tests/audio/check.h"
#include "wl/base/endian.h"

void wlt_suite_sseq(void);
void wlt_sseq_core(void);

#define CHK(name, cond) wlt_check((name), (cond), #cond)
#define CHKF(name, cond, ...)                                                                      \
    do {                                                                                           \
        char d_[160];                                                                              \
        (void)snprintf(d_, sizeof d_, __VA_ARGS__);                                                \
        wlt_check((name), (cond), d_);                                                             \
    } while (0)

// ---- a tiny assembler for SSEQ commands -------------------------------------------------------

typedef struct {
    uint8_t b[1536];
    uint32_t n;
} Asm;

static void a8(Asm *a, uint32_t v) {
    if (a->n < sizeof a->b) {
        a->b[a->n++] = (uint8_t)v;
    }
}
static void a16(Asm *a, uint32_t v) {
    a8(a, v & 255u);
    a8(a, v >> 8);
}
static void a24(Asm *a, uint32_t v) {
    a16(a, v & 0xFFFFu);
    a8(a, v >> 16);
}
static void avlq(Asm *a, uint32_t v) {
    uint8_t t[4];
    uint32_t n = 0;
    t[n++] = (uint8_t)(v & 127u);
    while ((v >>= 7) != 0) {
        t[n++] = (uint8_t)(128u | (v & 127u));
    }
    while (n > 0) {
        a8(a, t[--n]);
    }
}
static void note(Asm *a, uint32_t key, uint32_t vel, uint32_t dur) {
    a8(a, key);
    a8(a, vel);
    avlq(a, dur);
}
static void rest(Asm *a, uint32_t d) {
    a8(a, 0x80);
    avlq(a, d);
}
static void prog(Asm *a, uint32_t p) {
    a8(a, 0x81);
    avlq(a, p);
}
static void c1(Asm *a, uint32_t op, uint32_t v) {
    a8(a, op);
    a8(a, v);
}
static void pan_left(Asm *a) {
    c1(a, 0xC0, 0); // pan -64: hard left, unity gain in the left channel
}
static void varop(Asm *a, uint32_t op, uint32_t var, int32_t val) {
    a8(a, op);
    a8(a, var);
    a16(a, (uint32_t)val & 0xFFFFu);
}

// ---- the synthetic world: waves, bank, wave archive ---------------------------------------------

enum { W_PCM16 = 0, W_PCM8, W_ADPCM, W_PCM16_16K };
enum {
    P_PCM16 = 0,
    P_PCM8,
    P_ADPCM,
    P_PSG50,
    P_NOISE,
    P_DRUMS,
    P_SPLIT,
    P_PCM16_16K,
    P_PSG_ENV,
    P_PSG12,
    P_PSG87,
    P_EMPTY,
    P_COUNT
};

#define N16 8192u
#define N8 4096u
#define NAD 4096u

typedef struct {
    int16_t pcm16[N16];
    uint8_t b16[N16 * 2u];
    uint8_t b8[N8];
    uint8_t bad[4u + NAD / 2u];
    int16_t ad_ref[NAD];
    uint8_t b16k[4096u * 2u];
    uint8_t swar[N16 * 2u + N8 + NAD / 2u + 4096u * 2u + 256u];
    size_t swar_n;
    uint8_t sbnk[2048];
    size_t sbnk_n;
    WlSynWaveArc war;
    WlSynBank bank;
} World;

static World g_w;
static bool g_world_ok;

static bool build_world(void) {
    World *w = &g_w;
    memset(w, 0, sizeof *w);
    synfix_wave16(w->pcm16, N16, 1);
    for (uint32_t i = 0; i < N16; i++) {
        wl_wr_le16(w->b16 + 2u * i, (uint16_t)w->pcm16[i]);
    }
    for (uint32_t i = 0; i < 4096u; i++) {
        wl_wr_le16(w->b16k + 2u * i, (uint16_t)w->pcm16[i]);
    }
    for (uint32_t i = 0; i < N8; i++) {
        w->b8[i] = (uint8_t)((i * 37u + (i >> 3) * 11u) & 255u);
    }
    int16_t ad_pcm[NAD];
    synfix_wave16(ad_pcm, NAD, 3);
    const size_t adn = synfix_ima_encode(ad_pcm, NAD, w->bad, w->ad_ref);
    if (adn != 4u + NAD / 2u) {
        return false;
    }
    const SynfixWave waves[4] = {
        {1, false, 32000, N16 / 2u, 0, w->b16}, // PCM16, one shot
        {0, true, 32000, 256, 768, w->b8},      // PCM8, loop 1024..4095
        {2, true, 32000, 129, 384, w->bad},     // ADPCM, loop 1024..4095
        {1, false, 16000, 2048, 0, w->b16k},    // PCM16 at 16 kHz
    };
    w->swar_n = synfix_swar(w->swar, sizeof w->swar, waves, 4);
    SynfixInst in[P_COUNT];
    memset(in, 0, sizeof in);
    in[P_PCM16] = synfix_single(1, W_PCM16, 60, 127, 127, 127, 127);
    in[P_PCM8] = synfix_single(1, W_PCM8, 60, 127, 127, 127, 127);
    in[P_ADPCM] = synfix_single(1, W_ADPCM, 60, 127, 127, 127, 127);
    in[P_PSG50] = synfix_single(2, 3, 60, 127, 127, 127, 127);
    in[P_NOISE] = synfix_single(3, 0, 60, 127, 127, 127, 127);
    in[P_PCM16_16K] = synfix_single(1, W_PCM16_16K, 60, 127, 127, 127, 127);
    in[P_PSG_ENV] = synfix_single(2, 3, 60, 90, 80, 64, 90);
    in[P_PSG12] = synfix_single(2, 0, 60, 127, 127, 127, 127);
    in[P_PSG87] = synfix_single(2, 6, 60, 127, 127, 127, 127);
    SynfixInst *d = &in[P_DRUMS];
    d->kind = 16;
    d->lo = 36;
    d->hi = 39;
    d->count = 4;
    d->notes[0] = in[P_PCM16].notes[0];
    d->notes[1] = in[P_PCM8].notes[0];
    d->notes[2] = in[P_ADPCM].notes[0];
    d->notes[3] = in[P_PSG50].notes[0];
    d->notes[3].wave = 1;
    SynfixInst *k = &in[P_SPLIT];
    k->kind = 17;
    k->count = 3;
    k->split_end[0] = 59;
    k->split_end[1] = 71;
    k->split_end[2] = 127;
    k->notes[0] = in[P_PCM16].notes[0];
    k->notes[1] = in[P_PCM8].notes[0];
    k->notes[2] = in[P_ADPCM].notes[0];
    in[P_EMPTY].kind = 0;
    w->sbnk_n = synfix_sbnk(w->sbnk, sizeof w->sbnk, in, P_COUNT);
    if (w->swar_n == 0 || w->sbnk_n == 0) {
        return false;
    }
    if (wl_syn_waves_open(&w->war, w->swar, w->swar_n) != WL_OK ||
        wl_syn_bank_open(&w->bank, w->sbnk, w->sbnk_n) != WL_OK) {
        return false;
    }
    wl_syn_bank_set_waves(&w->bank, 0, &w->war);
    return true;
}

// ---- running a sequence -------------------------------------------------------------------------

#define MAXEV 4096u
typedef struct {
    WlSynEvent e;
    uint32_t frame; // engine frames rendered when it fired
} Ev;

static Ev g_ev[MAXEV];
static uint32_t g_nev;
static WlSynth *g_cur;

static void trace_fn(void *user, const WlSynEvent *e) {
    (void)user;
    if (g_nev < MAXEV) {
        g_ev[g_nev].e = *e;
        g_ev[g_nev].frame = g_cur->stats.frames;
        g_nev++;
    }
}

static uint32_t ev_count(uint32_t kind) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nev; i++) {
        n += g_ev[i].e.kind == kind ? 1u : 0u;
    }
    return n;
}

// The n-th (0-based) event of a kind, or NULL.
static const Ev *ev_nth(uint32_t kind, uint32_t n) {
    for (uint32_t i = 0; i < g_nev; i++) {
        if (g_ev[i].e.kind == kind && n-- == 0) {
            return &g_ev[i];
        }
    }
    return NULL;
}

static WlSynth g_syn;
static WlSynSeq g_seq;
static uint8_t g_sseq[2048];
#define PIECE_FRAMES 64000u
static int16_t g_out[2u * PIECE_FRAMES];

static bool start(const Asm *a, const WlSynParams *par) {
    const size_t n = synfix_sseq(g_sseq, sizeof g_sseq, a->b, a->n);
    WlSynParams p;
    if (par != NULL) {
        p = *par;
    } else {
        wl_syn_params_default(&p);
    }
    g_nev = 0;
    if (n == 0 || wl_syn_seq_open(&g_seq, g_sseq, n) != WL_OK ||
        wl_syn_init(&g_syn, &g_seq, &g_w.bank, &p) != WL_OK) {
        return false;
    }
    g_cur = &g_syn;
    wl_syn_set_trace(&g_syn, trace_fn, NULL);
    return true;
}

static void run(uint32_t frames) {
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = frames - done > 100u ? 100u : frames - done;
        wl_syn_render(&g_syn, g_out + 2u * done, n);
        done += n;
    }
}

// Renders enough frames for `t` ticks at tempo 120 (tick n fires in frame 2n - 1) and a few more.
static void run_ticks(uint32_t t) {
    run((2u * t + 3u) * 167u);
}

// Renders until engine frame `f` has just started (the state after frame f's update, before its
// audio is mixed is not observable; this renders one sample of it).
static void run_to_frame(uint32_t f) {
    while (g_syn.stats.frames < f && !g_syn.done) {
        int16_t t[2];
        wl_syn_render(&g_syn, t, 1);
    }
}

// First sample of engine frame f (1-based): floor(f * 32000 / 192) with the frame numbering of the
// synth.
static uint32_t frame_start(uint32_t f) {
    return (f - 1u) * WL_SYN_RATE / WL_SYN_FRAME_HZ;
}

static uint32_t fnv(const int16_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ (uint16_t)p[i]) * 16777619u;
    }
    return h;
}

// ---- table and parser checks --------------------------------------------------------------------

static void t_tables(void) {
    CHK("sseq_db_full", wl_syn_volume_db(127) == 0);
    CHK("sseq_db_zero", wl_syn_volume_db(0) == -723);
    CHKF("sseq_db_64", wl_syn_volume_db(64) == -119, "got %d", (int)wl_syn_volume_db(64));
    CHKF("sseq_db_square_law", wl_syn_volume_db(32) < wl_syn_volume_db(64) - 100, "%d vs %d",
         (int)wl_syn_volume_db(32), (int)wl_syn_volume_db(64));
    CHK("sseq_gain_unity", wl_syn_level_gain(0) == 65536u);
    const uint32_t half = wl_syn_level_gain(-7706); // one octave of amplitude: 6.02 dB
    CHKF("sseq_gain_octave", half > 32768u - 60u && half < 32768u + 60u, "got %u", half);
    CHK("sseq_gain_silent", wl_syn_level_gain(-92544) == 0 && wl_syn_level_gain(-100000) == 0);
    bool mono = true;
    uint32_t prev = 65536u;
    for (int32_t l = -1; l > -92544; l -= 97) {
        const uint32_t g = wl_syn_level_gain(l);
        mono = mono && g <= prev;
        prev = g;
    }
    CHK("sseq_gain_monotonic", mono);
    CHK("sseq_gain_decibels",
        fabs((double)wl_syn_level_gain(-60 * 128) / 65536.0 - pow(10.0, -60.0 / 200.0)) < 0.002);
    CHK("sseq_step_unity", wl_syn_step(32000, 0) == 65536u);
    CHK("sseq_step_octave_up", wl_syn_step(32000, 768) == 131072u);
    CHK("sseq_step_octave_down", wl_syn_step(32000, -768) == 32768u);
    CHK("sseq_step_rate", wl_syn_step(16000, 0) == 32768u && wl_syn_step(32728, 0) == 67027u);
    CHK("sseq_step_fifth", wl_syn_step(32000, 7 * 64) == 98193u);
    CHK("sseq_step_clamped",
        wl_syn_step(32000, 768 * 7) == (16u << 16) && wl_syn_step(32000, -768 * 7) == 1024u);
    bool pitch_ok = true;
    for (int32_t p = -1000; p <= 1000; p += 37) {
        const double want = 65536.0 * pow(2.0, (double)p / 768.0);
        const double got = (double)wl_syn_step(32000, p);
        pitch_ok = pitch_ok && fabs(got - want) <= want * 2e-4 + 1.0;
    }
    CHK("sseq_step_curve", pitch_ok);
}

static void t_parsers(void) {
    WlSynSeq q;
    uint8_t f[64];
    const uint8_t cmds[4] = {0x80, 0x10, 0xFF, 0xFF};
    const size_t n = synfix_sseq(f, sizeof f, cmds, sizeof cmds);
    CHK("sseq_seq_open",
        wl_syn_seq_open(&q, f, n) == WL_OK && q.data_size == 4 && q.data == f + 0x1C);
    CHK("sseq_seq_null",
        wl_syn_seq_open(NULL, f, n) != WL_OK && wl_syn_seq_open(&q, NULL, 0) != WL_OK);
    CHK("sseq_seq_short", wl_syn_seq_open(&q, f, 10) != WL_OK);
    uint8_t bad[64];
    memcpy(bad, f, n);
    bad[0] = 'X';
    CHK("sseq_seq_magic", wl_syn_seq_open(&q, bad, n) != WL_OK);
    memcpy(bad, f, n);
    wl_wr_le32(bad + 24, (uint32_t)n + 4u);
    CHK("sseq_seq_data_offset", wl_syn_seq_open(&q, bad, n) != WL_OK);
    memcpy(bad, f, n);
    wl_wr_le32(bad + 8, (uint32_t)n + 100u);
    CHK("sseq_seq_size", wl_syn_seq_open(&q, bad, n) != WL_OK);
    memcpy(bad, f, n);
    wl_wr_le16(bad + 4, 0xFFFEu);
    CHK("sseq_seq_bom", wl_syn_seq_open(&q, bad, n) != WL_OK);

    CHK("sseq_world", g_world_ok);
    WlSynWave wv;
    CHK("sseq_wave_pcm16", wl_syn_wave_get(&g_w.war, W_PCM16, &wv) && wv.enc == 1 &&
                               wv.nsamples == N16 && !wv.loop && wv.rate == 32000);
    CHK("sseq_wave_pcm8_loop", wl_syn_wave_get(&g_w.war, W_PCM8, &wv) && wv.enc == 0 &&
                                   wv.nsamples == N8 && wv.loop && wv.loop_start == 1024);
    CHK("sseq_wave_adpcm_loop", wl_syn_wave_get(&g_w.war, W_ADPCM, &wv) && wv.enc == 2 &&
                                    wv.nsamples == NAD && wv.loop && wv.loop_start == 1024);
    CHK("sseq_wave_oob", !wl_syn_wave_get(&g_w.war, 4, &wv) && !wl_syn_wave_get(NULL, 0, &wv));
    static uint8_t swar[sizeof g_w.swar];
    memcpy(swar, g_w.swar, g_w.swar_n);
    WlSynWaveArc wa;
    CHK("sseq_swar_open", wl_syn_waves_open(&wa, swar, g_w.swar_n) == WL_OK && wa.count == 4);
    wl_wr_le32(swar + 0x3C + 4, (uint32_t)g_w.swar_n - 8u); // wave 1 starts too close to the end
    CHK("sseq_wave_truncated",
        wl_syn_waves_open(&wa, swar, g_w.swar_n) == WL_OK && !wl_syn_wave_get(&wa, 1, &wv));
    memcpy(swar, g_w.swar, g_w.swar_n);
    wl_wr_le32(swar + 0x38, 100000u);
    CHK("sseq_swar_count", wl_syn_waves_open(&wa, swar, g_w.swar_n) != WL_OK);
    memcpy(swar, g_w.swar, g_w.swar_n);
    swar[wl_rd_le32(swar + 0x3C)] = 9; // unknown encoding
    CHK("sseq_wave_encoding",
        wl_syn_waves_open(&wa, swar, g_w.swar_n) == WL_OK && !wl_syn_wave_get(&wa, 0, &wv));

    // Instruments.
    WlSynNote nt;
    CHK("sseq_inst_single", wl_syn_bank_note(&g_w.bank, P_PCM16, 0, &nt) &&
                                nt.type == WL_SYN_NOTE_PCM && nt.wave == W_PCM16 && nt.key == 60 &&
                                nt.pan == 0);
    CHK("sseq_inst_single_any_key",
        wl_syn_bank_note(&g_w.bank, P_PCM16, 127, &nt) && nt.wave == W_PCM16);
    CHK("sseq_inst_psg", wl_syn_bank_note(&g_w.bank, P_PSG50, 60, &nt) &&
                             nt.type == WL_SYN_NOTE_PSG && nt.wave == 3);
    CHK("sseq_inst_noise",
        wl_syn_bank_note(&g_w.bank, P_NOISE, 60, &nt) && nt.type == WL_SYN_NOTE_NOISE);
    CHK("sseq_inst_adsr", wl_syn_bank_note(&g_w.bank, P_PSG_ENV, 60, &nt) && nt.attack == 90 &&
                              nt.decay == 80 && nt.sustain == 64 && nt.release == 90);
    CHK("sseq_inst_drum_keys",
        wl_syn_bank_note(&g_w.bank, P_DRUMS, 36, &nt) && nt.wave == W_PCM16 &&
            wl_syn_bank_note(&g_w.bank, P_DRUMS, 37, &nt) && nt.wave == W_PCM8 &&
            wl_syn_bank_note(&g_w.bank, P_DRUMS, 38, &nt) && nt.wave == W_ADPCM &&
            wl_syn_bank_note(&g_w.bank, P_DRUMS, 39, &nt) && nt.type == WL_SYN_NOTE_PSG);
    CHK("sseq_inst_drum_range", !wl_syn_bank_note(&g_w.bank, P_DRUMS, 35, &nt) &&
                                    !wl_syn_bank_note(&g_w.bank, P_DRUMS, 40, &nt));
    CHK("sseq_inst_split",
        wl_syn_bank_note(&g_w.bank, P_SPLIT, 0, &nt) && nt.wave == W_PCM16 &&
            wl_syn_bank_note(&g_w.bank, P_SPLIT, 59, &nt) && nt.wave == W_PCM16 &&
            wl_syn_bank_note(&g_w.bank, P_SPLIT, 60, &nt) && nt.wave == W_PCM8 &&
            wl_syn_bank_note(&g_w.bank, P_SPLIT, 71, &nt) && nt.wave == W_PCM8 &&
            wl_syn_bank_note(&g_w.bank, P_SPLIT, 72, &nt) && nt.wave == W_ADPCM &&
            wl_syn_bank_note(&g_w.bank, P_SPLIT, 127, &nt) && nt.wave == W_ADPCM);
    CHK("sseq_inst_empty", !wl_syn_bank_note(&g_w.bank, P_EMPTY, 60, &nt) &&
                               !wl_syn_bank_note(&g_w.bank, 99, 60, &nt) &&
                               !wl_syn_bank_note(&g_w.bank, 0, 128, &nt));
    // A stored 255 reads as 127 (the envelope parameters are 7 bits).
    static uint8_t sb[sizeof g_w.sbnk];
    memcpy(sb, g_w.sbnk, g_w.sbnk_n);
    WlSynBank bk;
    const uint32_t off = wl_rd_le16(sb + 0x3C + 4u * P_PCM16 + 1);
    sb[off + 5] = 255;
    CHK("sseq_inst_adsr_clamped", wl_syn_bank_open(&bk, sb, g_w.sbnk_n) == WL_OK &&
                                      wl_syn_bank_note(&bk, P_PCM16, 60, &nt) && nt.attack == 127);
    wl_wr_le16(sb + off + 2, 7); // a wave slot that does not exist
    CHK("sseq_inst_slot_range", !wl_syn_bank_note(&bk, P_PCM16, 60, &nt));
    CHK("sseq_bank_header", wl_syn_bank_open(&bk, g_w.sbnk, 20) != WL_OK);

    WlSynBankCheck bc;
    CHK("sseq_bank_check", wl_syn_bank_check(&g_w.bank, &bc) == WL_OK &&
                               bc.instruments == P_COUNT - 1u && bc.unresolved == 0 &&
                               bc.bad == 0 && bc.pcm_notes == 10u);
    WlSynBank nowaves = g_w.bank;
    wl_syn_bank_set_waves(&nowaves, 0, NULL);
    CHKF("sseq_bank_check_unresolved",
         wl_syn_bank_check(&nowaves, &bc) == WL_OK && bc.unresolved == 10u, "unresolved %u",
         bc.unresolved);
}

// ---- wave playback exactness
// -----------------------------------------------------------------------

// Plays program `p` at `key` for a long note, hard left at unity gain: the left channel is then the
// wave itself (the envelope is open and the pan gives gain 1.0).
static bool play_wave(uint32_t p, uint32_t key, bool nearest) {
    Asm a = {{0}, 0};
    pan_left(&a);
    prog(&a, p);
    note(&a, key, 127, 96);
    a8(&a, 0xFF);
    WlSynParams par;
    wl_syn_params_default(&par);
    par.nearest = nearest;
    return start(&a, &par);
}

#define SETTLE 200u // samples: past the first frame's gain ramp

static void t_wave_exact(void) {
    CHK("sseq_exact_start", play_wave(P_PCM16, 60, false));
    run(9000);
    bool ok = true;
    uint32_t bad = 0;
    for (uint32_t i = SETTLE; i < N16; i++) {
        if (g_out[2u * i] != g_w.pcm16[i] || g_out[2u * i + 1u] != 0) {
            ok = false;
            bad = i;
            break;
        }
    }
    CHKF("sseq_pcm16_exact", ok, "first difference at %u", bad);
    ok = true;
    for (uint32_t i = N16; i < 9000; i++) {
        ok = ok && g_out[2u * i] == 0;
    }
    CHK("sseq_pcm16_one_shot_ends", ok);
    const uint32_t ended = ev_count(WL_SYN_EV_VOICE_OFF);
    CHKF("sseq_pcm16_voice_off", ended == 1u, "voice off events %u", ended);

    // PCM8 with a loop: 0..1023 once, then 1024..4095 forever.
    CHK("sseq_pcm8_start", play_wave(P_PCM8, 60, false));
    run(20000);
    ok = true;
    bad = 0;
    for (uint32_t i = SETTLE; i < 20000u; i++) {
        const uint32_t idx = i < N8 ? i : 1024u + (i - 1024u) % 3072u;
        const int32_t want = (int32_t)(int8_t)g_w.b8[idx] * 256;
        if (g_out[2u * i] != want) {
            ok = false;
            bad = i;
            break;
        }
    }
    CHKF("sseq_pcm8_loop_exact", ok, "first difference at %u", bad);

    // IMA-ADPCM with a loop: the decoder restarts from the state it had at the loop start.
    CHK("sseq_adpcm_start", play_wave(P_ADPCM, 60, false));
    run(20000);
    ok = true;
    bad = 0;
    for (uint32_t i = SETTLE; i < 20000u; i++) {
        const uint32_t idx = i < NAD ? i : 1024u + (i - 1024u) % 3072u;
        if (g_out[2u * i] != g_w.ad_ref[idx]) {
            ok = false;
            bad = i;
            break;
        }
    }
    CHKF("sseq_adpcm_loop_exact", ok, "first difference at %u", bad);
    // The reference decoder of the audio engine agrees with the encoder's own reconstruction.
    WlImaState st;
    int16_t dec[NAD];
    wl_ima_init(&st, 0, 0);
    wl_ima_decode(&st, g_w.bad + 4, NAD, dec);
    CHK("sseq_adpcm_reference", memcmp(dec, g_w.ad_ref, sizeof dec) == 0);

    // Pitch: an octave up takes every second sample; an octave down interpolates between
    // neighbours.
    CHK("sseq_up_start", play_wave(P_PCM16, 72, false));
    run(5000);
    ok = true;
    bad = 0;
    for (uint32_t i = SETTLE; i < 4000u; i++) {
        if (g_out[2u * i] != g_w.pcm16[2u * i]) {
            ok = false;
            bad = i;
            break;
        }
    }
    CHKF("sseq_octave_up_exact", ok, "first difference at %u", bad);
    CHK("sseq_down_start", play_wave(P_PCM16, 48, false));
    run(9000);
    ok = true;
    bad = 0;
    for (uint32_t i = SETTLE; i < 8000u; i++) {
        const uint32_t j = i / 2u;
        int32_t want = g_w.pcm16[j];
        if (i & 1u) {
            want += (((int32_t)g_w.pcm16[j + 1] - want) * 2048) >> 12;
        }
        if (g_out[2u * i] != want) {
            ok = false;
            bad = i;
            break;
        }
    }
    CHKF("sseq_octave_down_interpolated", ok, "first difference at %u", bad);
    CHK("sseq_nearest_start", play_wave(P_PCM16, 48, true));
    run(9000);
    ok = true;
    for (uint32_t i = SETTLE; i < 8000u; i++) {
        ok = ok && g_out[2u * i] == g_w.pcm16[i / 2u];
    }
    CHK("sseq_nearest_option", ok);
    // A 16 kHz wave plays at half speed: the same as an octave down.
    CHK("sseq_rate_start", play_wave(P_PCM16_16K, 60, true));
    run(9000);
    ok = true;
    for (uint32_t i = SETTLE; i < 8000u; i++) {
        ok = ok && g_out[2u * i] == g_w.pcm16[i / 2u];
    }
    CHK("sseq_wave_rate", ok);
    // Instrument choice through programs, drum keys and key splits: which wave a voice reads.
    WlSynWave wv;
    {
        Asm a = {{0}, 0};
        pan_left(&a);
        c1(&a, 0xC7, 0); // chords
        prog(&a, P_DRUMS);
        note(&a, 36, 100, 96);
        note(&a, 37, 100, 96);
        note(&a, 38, 100, 96);
        note(&a, 39, 100, 96);
        note(&a, 40, 100, 96); // outside the drum set: silent
        prog(&a, P_SPLIT);
        note(&a, 10, 100, 96);
        note(&a, 65, 100, 96);
        note(&a, 100, 100, 96);
        a8(&a, 0xFF);
        CHK("sseq_inst_play_start", start(&a, NULL));
        run(400);
        const WlSynVoice *v = g_syn.voice;
        bool ok2 = ev_count(WL_SYN_EV_NOTE_ON) == 7u;
        ok2 = ok2 && wl_syn_wave_get(&g_w.war, W_PCM16, &wv) && v[0].data == wv.data &&
              v[0].kind == 1;
        ok2 =
            ok2 && wl_syn_wave_get(&g_w.war, W_PCM8, &wv) && v[1].data == wv.data && v[1].kind == 0;
        ok2 = ok2 && wl_syn_wave_get(&g_w.war, W_ADPCM, &wv) && v[2].data == wv.data &&
              v[2].kind == 2;
        ok2 = ok2 && v[3].kind == 3 && v[3].duty == 1;
        ok2 = ok2 && g_syn.stats.notes_silent == 1u;
        wl_syn_wave_get(&g_w.war, W_PCM16, &wv);
        ok2 = ok2 && v[4].data == wv.data;
        wl_syn_wave_get(&g_w.war, W_PCM8, &wv);
        ok2 = ok2 && v[5].data == wv.data;
        wl_syn_wave_get(&g_w.war, W_ADPCM, &wv);
        ok2 = ok2 && v[6].data == wv.data;
        CHK("sseq_inst_play", ok2);
    }
}

// ---- levels, pan, envelopes
// ----------------------------------------------------------------------

// Largest |left| in the last 4 samples of engine frame f (the gain has ramped to that frame's
// target).
static int32_t frame_peak_l(uint32_t f) {
    const uint32_t end = frame_start(f + 1u);
    int32_t pk = 0;
    for (uint32_t i = end - 4u; i < end; i++) {
        const int32_t v = g_out[2u * i];
        pk = pk < v ? v : pk < -v ? -v : pk;
    }
    return pk;
}

static int32_t abs32(int32_t v) {
    return v < 0 ? -v : v;
}

static double amp_of(double level_q7) {
    return 32767.0 * pow(10.0, level_q7 / 128.0 / 200.0);
}

static bool near_amp(int32_t got, double want) {
    return fabs((double)got - want) <= want * 0.02 + 6.0;
}

static void t_envelope(void) {
    WlSynParams pn;
    wl_syn_params_default(&pn);
    pn.nearest = true; // square edges stay sharp, so the last samples of a frame show its gain
    // Open envelope: full amplitude.
    Asm a = {{0}, 0};
    pan_left(&a);
    prog(&a, P_PSG50);
    note(&a, 69, 127, 96);
    a8(&a, 0xFF);
    CHK("sseq_env_start", start(&a, NULL));
    run(3000);
    CHKF("sseq_level_full", frame_peak_l(10) == 32767, "peak %d", (int)frame_peak_l(10));

    // Velocity, track volume, expression, master volume and the sequence volume all follow the
    // square law.
    struct {
        const char *name;
        uint32_t op, val;
    } lv[] = {{"sseq_level_track_volume", 0xC1, 64},
              {"sseq_level_expression", 0xD5, 64},
              {"sseq_level_master", 0xC2, 64}};
    for (uint32_t i = 0; i < 3; i++) {
        Asm b = {{0}, 0};
        pan_left(&b);
        c1(&b, lv[i].op, lv[i].val);
        prog(&b, P_PSG50);
        note(&b, 69, 127, 96);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(3000);
        CHKF(lv[i].name, near_amp(frame_peak_l(10), amp_of(-119.0 * 128.0)), "peak %d want %.0f",
             (int)frame_peak_l(10), amp_of(-119.0 * 128.0));
    }
    {
        Asm b = {{0}, 0};
        pan_left(&b);
        prog(&b, P_PSG50);
        note(&b, 69, 64, 96);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(3000);
        CHKF("sseq_level_velocity", near_amp(frame_peak_l(10), amp_of(-119.0 * 128.0)), "peak %d",
             (int)frame_peak_l(10));
        Asm sv = {{0}, 0};
        pan_left(&sv);
        prog(&sv, P_PSG50);
        note(&sv, 69, 127, 96);
        a8(&sv, 0xFF);
        WlSynParams par;
        wl_syn_params_default(&par);
        par.volume = 64;
        par.nearest = true;
        start(&sv, &par);
        run(3000);
        CHKF("sseq_level_sequence_volume", near_amp(frame_peak_l(10), amp_of(-119.0 * 128.0)),
             "peak %d", (int)frame_peak_l(10));
        // Levels add: velocity 64 and volume 64 is twice the decibels.
        Asm c = {{0}, 0};
        pan_left(&c);
        c1(&c, 0xC1, 64);
        prog(&c, P_PSG50);
        note(&c, 69, 64, 96);
        a8(&c, 0xFF);
        start(&c, &pn);
        run(3000);
        CHKF("sseq_level_adds", near_amp(frame_peak_l(10), amp_of(-238.0 * 128.0)),
             "peak %d want %.0f", (int)frame_peak_l(10), amp_of(-238.0 * 128.0));
    }

    // Pan: centre is half amplitude on each side; right is the mirror image of left.
    {
        Asm b = {{0}, 0};
        prog(&b, P_PSG50);
        note(&b, 69, 127, 96);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(3000);
        const uint32_t end = frame_start(11u);
        int32_t pl = 0, pr = 0;
        for (uint32_t i = end - 24u; i < end; i++) {
            pl = pl < abs32(g_out[2u * i]) ? abs32(g_out[2u * i]) : pl;
            pr = pr < abs32(g_out[2u * i + 1u]) ? abs32(g_out[2u * i + 1u]) : pr;
        }
        CHKF("sseq_pan_centre", pl == pr && pl > 16000 && pl < 16400, "L %d R %d", (int)pl,
             (int)pr);
        Asm c = {{0}, 0};
        c1(&c, 0xC0, 127);
        prog(&c, P_PSG50);
        note(&c, 69, 127, 96);
        a8(&c, 0xFF);
        start(&c, &pn);
        run(3000);
        pl = pr = 0;
        for (uint32_t i = end - 24u; i < end; i++) {
            pl = pl < abs32(g_out[2u * i]) ? abs32(g_out[2u * i]) : pl;
            pr = pr < abs32(g_out[2u * i + 1u]) ? abs32(g_out[2u * i + 1u]) : pr;
        }
        CHKF("sseq_pan_right", pr > 32000 && pl < 300, "L %d R %d", (int)pl, (int)pr);
    }

    // Sustain level: 64 settles at (64/127)^2.
    {
        Asm b = {{0}, 0};
        pan_left(&b);
        c1(&b, 0xD2, 64);
        prog(&b, P_PSG50);
        note(&b, 69, 127, 96);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(8000);
        CHKF("sseq_env_sustain", near_amp(frame_peak_l(30), amp_of(-119.0 * 128.0)), "peak %d",
             (int)frame_peak_l(30));
    }

    // Decay 100 falls 0x1E00 / 26 = 295 units per frame towards a sustain of 32 (-210 dB10).
    {
        Asm b = {{0}, 0};
        pan_left(&b);
        c1(&b, 0xD1, 100);
        c1(&b, 0xD2, 32);
        prog(&b, P_PSG50);
        note(&b, 69, 127, 192);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(12000);
        bool ok = true;
        for (uint32_t f = 3; f < 60u; f += 4) {
            const double lvl = -295.0 * (double)(f - 1u);
            const double want = lvl < -210.0 * 128.0 ? amp_of(-210.0 * 128.0) : amp_of(lvl);
            ok = ok && near_amp(frame_peak_l(f), want);
        }
        CHK("sseq_env_decay", ok);
    }

    // Attack 64: the level is multiplied by 191 / 256 every frame (integer arithmetic), from
    // -92544.
    {
        Asm b = {{0}, 0};
        pan_left(&b);
        c1(&b, 0xD0, 64);
        prog(&b, P_PSG50);
        note(&b, 69, 127, 192);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(21000);
        int32_t env = -92544;
        bool ok = true;
        for (uint32_t f = 1; f <= 120u; f++) {
            env = env * 191 / 256;
            if (f % 5u == 0 && env < 0) {
                ok = ok && near_amp(frame_peak_l(f), amp_of((double)env));
            }
        }
        CHK("sseq_env_attack_curve", ok);
        // Attack 127 is instant, 109 follows the table (0x8F), 127 - 18 = 109.
        Asm c = {{0}, 0};
        pan_left(&c);
        c1(&c, 0xD0, 109);
        prog(&c, P_PSG50);
        note(&c, 69, 127, 192);
        a8(&c, 0xFF);
        start(&c, &pn);
        run(12000);
        env = -92544;
        ok = true;
        for (uint32_t f = 1; f <= 40u; f++) {
            env = env * 0x8F / 256;
            if (f % 4u == 0 && env < 0) {
                ok = ok && near_amp(frame_peak_l(f), amp_of((double)env));
            }
        }
        CHK("sseq_env_attack_table", ok);
    }

    // Release 100 (295 per frame) from the sustain level after a note of 48 ticks (released in
    // frame 97).
    {
        Asm b = {{0}, 0};
        pan_left(&b);
        c1(&b, 0xD2, 64);
        c1(&b, 0xD3, 100);
        prog(&b, P_PSG50);
        note(&b, 69, 127, 48);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(62000);
        const Ev *rel = ev_nth(WL_SYN_EV_RELEASE, 0);
        CHKF("sseq_env_release_frame", rel != NULL && rel->frame == 97u, "release at frame %u",
             rel != NULL ? rel->frame : 0u);
        bool ok = rel != NULL;
        for (uint32_t j = 2; ok && j < 40u; j += 6) {
            const double lvl = -119.0 * 128.0 - 295.0 * (double)(j + 1u);
            ok = near_amp(frame_peak_l(97u + j), amp_of(lvl));
        }
        CHK("sseq_env_release_curve", ok);
        const Ev *off = ev_nth(WL_SYN_EV_VOICE_OFF, 0);
        const uint32_t frames_needed = (uint32_t)((92544 - 119 * 128 + 294) / 295);
        CHKF("sseq_env_release_end", off != NULL && off->frame == 97u + frames_needed - 1u,
             "off at %u want %u", off != NULL ? off->frame : 0u, 97u + frames_needed - 1u);
        CHK("sseq_done_after_release", wl_syn_done(&g_syn));
    }

    // The instrument's own envelope (A 90, D 80, S 64, R 90) applies when the sequence does not
    // override it.
    {
        Asm b = {{0}, 0};
        pan_left(&b);
        prog(&b, P_PSG_ENV);
        note(&b, 69, 127, 192);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(26000);
        int32_t env = -92544;
        const int32_t mult = 255 - 90;
        const int32_t decay = (int32_t)(0x1E00 / (126 - 80));
        bool ok = true;
        bool attacking = true;
        for (uint32_t f = 1; f <= 150u; f++) {
            if (attacking) {
                env = env * mult / 256;
                if (env >= 0) {
                    env = 0;
                    attacking = false;
                }
            } else if (env > -119 * 128) {
                env -= decay;
                if (env < -119 * 128) {
                    env = -119 * 128;
                }
            }
            if (f % 7u == 0) {
                ok = ok && near_amp(frame_peak_l(f), amp_of((double)env));
            }
        }
        CHK("sseq_env_instrument_adsr", ok);
    }
    // A sequence override wins over the instrument.
    {
        Asm b = {{0}, 0};
        pan_left(&b);
        c1(&b, 0xD0, 127);
        c1(&b, 0xD1, 127);
        c1(&b, 0xD2, 127);
        prog(&b, P_PSG_ENV);
        note(&b, 69, 127, 192);
        a8(&b, 0xFF);
        start(&b, &pn);
        run(5000);
        CHKF("sseq_env_override", frame_peak_l(8) == 32767, "peak %d", (int)frame_peak_l(8));
    }
}

// ---- PSG and noise -----------------------------------------------------------------------------

static void t_psg(void) {
    const struct {
        uint32_t prog, key;
        double hz;
    } t[] = {
        {P_PSG50, 69, 440.0}, {P_PSG50, 81, 880.0}, {P_PSG50, 57, 220.0}, {P_PSG50, 60, 261.63}};
    for (uint32_t i = 0; i < 4; i++) {
        Asm a = {{0}, 0};
        pan_left(&a);
        prog(&a, t[i].prog);
        note(&a, t[i].key, 127, 96);
        a8(&a, 0xFF);
        start(&a, NULL);
        run(24000);
        uint32_t edges = 0;
        for (uint32_t k = 4001; k < 24000u; k++) {
            edges += (g_out[2u * (k - 1u)] < 0 && g_out[2u * k] >= 0) ? 1u : 0u;
        }
        const double want = t[i].hz * 20000.0 / 32000.0;
        char nm[40];
        (void)snprintf(nm, sizeof nm, "sseq_psg_pitch_%u", t[i].key);
        CHKF(nm, fabs((double)edges - want) <= 1.5, "%u rising edges, want %.1f", edges, want);
    }
    const struct {
        uint32_t prog;
        double high;
    } d[] = {{P_PSG12, 0.125}, {P_PSG50, 0.5}, {P_PSG87, 0.875}};
    for (uint32_t i = 0; i < 3; i++) {
        Asm a = {{0}, 0};
        pan_left(&a);
        prog(&a, d[i].prog);
        note(&a, 57, 127, 96); // 220 Hz: 145 samples per cycle
        a8(&a, 0xFF);
        start(&a, NULL);
        run(16000);
        uint32_t hi = 0;
        for (uint32_t k = 4000; k < 16000u; k++) {
            hi += g_out[2u * k] > 0 ? 1u : 0u;
        }
        char nm[40];
        (void)snprintf(nm, sizeof nm, "sseq_psg_duty_%u", (unsigned)(d[i].high * 1000.0));
        CHKF(nm, fabs((double)hi / 12000.0 - d[i].high) < 0.02, "high fraction %.3f",
             (double)hi / 12000.0);
    }
    Asm a = {{0}, 0};
    pan_left(&a);
    prog(&a, P_NOISE);
    note(&a, 69, 127, 96);
    a8(&a, 0xFF);
    start(&a, NULL);
    run(16000);
    uint32_t changes = 0, pos = 0;
    for (uint32_t k = 4001; k < 16000u; k++) {
        changes += ((g_out[2u * k] > 0) != (g_out[2u * (k - 1u)] > 0)) ? 1u : 0u;
        pos += g_out[2u * k] > 0 ? 1u : 0u;
    }
    CHKF("sseq_noise", changes > 300u && pos > 4000u && pos < 8000u, "changes %u positive %u",
         changes, pos);
}

// ---- sequencer ---------------------------------------------------------------------------------

static void t_timing(void) {
    Asm a = {{0}, 0};
    prog(&a, P_PSG50);
    note(&a, 60, 100, 10);
    rest(&a, 38); // 48 ticks from the first note to the second
    note(&a, 62, 100, 10);
    a8(&a, 0xFF);
    start(&a, NULL);
    run(40000);
    const Ev *n0 = ev_nth(WL_SYN_EV_NOTE_ON, 0);
    const Ev *n1 = ev_nth(WL_SYN_EV_NOTE_ON, 1);
    CHK("sseq_time_two_notes", n0 != NULL && n1 != NULL);
    if (n0 != NULL && n1 != NULL) {
        CHKF("sseq_time_first_tick", n0->e.tick == 1u && n0->frame == 1u, "tick %u frame %u",
             n0->e.tick, n0->frame);
        CHKF("sseq_time_tempo120", n1->e.tick == 49u && n1->frame - n0->frame == 96u,
             "tick %u, %u frames apart", n1->e.tick, n1->frame - n0->frame);
        CHK("sseq_time_note_args",
            n0->e.a == 60 && n0->e.b == 100 && n0->e.c == 10 && n1->e.a == 62);
    }
    // 96 ticks per second at 120 BPM.
    Asm b = {{0}, 0};
    rest(&b, 1);
    a8(&b, 0x94);
    a24(&b, 0);
    start(&b, NULL);
    run(32000);
    CHKF("sseq_time_ticks_per_second", g_syn.stats.ticks == 96u, "ticks %u", g_syn.stats.ticks);
    // Tempo 240 (E1 = u16): twice as many ticks.
    Asm c = {{0}, 0};
    a8(&c, 0xE1);
    a16(&c, 240);
    rest(&c, 1);
    a8(&c, 0x94);
    a24(&c, 3);
    start(&c, NULL);
    run(32000);
    CHKF("sseq_time_tempo240", g_syn.stats.ticks >= 191u && g_syn.stats.ticks <= 193u, "ticks %u",
         g_syn.stats.ticks);
    // The tempo parameter scales the clock.
    WlSynParams par;
    wl_syn_params_default(&par);
    par.tempo_q8 = 128;
    start(&b, &par);
    run(32000);
    CHKF("sseq_time_tempo_scale", g_syn.stats.ticks == 48u, "ticks %u", g_syn.stats.ticks);
    // A tempo change in the middle: 48 ticks at 120, then the rest at 240.
    Asm d = {{0}, 0};
    prog(&d, P_PSG50);
    rest(&d, 48);
    a8(&d, 0xE1);
    a16(&d, 240);
    note(&d, 60, 100, 4);
    rest(&d, 48);
    note(&d, 62, 100, 4);
    a8(&d, 0xFF);
    start(&d, NULL);
    run(40000);
    n0 = ev_nth(WL_SYN_EV_NOTE_ON, 0);
    n1 = ev_nth(WL_SYN_EV_NOTE_ON, 1);
    CHKF("sseq_time_tempo_change",
         n0 != NULL && n1 != NULL && n1->e.tick - n0->e.tick == 52u &&
             n1->frame - n0->frame >= 51u && n1->frame - n0->frame <= 53u,
         "%u ticks %u frames apart", n1 != NULL ? n1->e.tick - n0->e.tick : 0u,
         n1 != NULL ? n1->frame - n0->frame : 0u);
}

static void t_commands(void) {
    // Chords: note wait off makes notes start together and the track continue.
    {
        Asm a = {{0}, 0};
        c1(&a, 0xC7, 0);
        prog(&a, P_PSG50);
        note(&a, 60, 100, 48);
        note(&a, 64, 100, 48);
        note(&a, 67, 100, 48);
        rest(&a, 48);
        note(&a, 72, 100, 48);
        a8(&a, 0xFF);
        start(&a, NULL);
        run_ticks(52);
        CHK("sseq_chord", ev_count(WL_SYN_EV_NOTE_ON) == 4u &&
                              ev_nth(WL_SYN_EV_NOTE_ON, 2)->e.tick == 1u &&
                              ev_nth(WL_SYN_EV_NOTE_ON, 3)->e.tick == 49u);
    }
    // With note wait on (the default) a note holds the track for its length.
    {
        Asm a = {{0}, 0};
        prog(&a, P_PSG50);
        note(&a, 60, 100, 20);
        note(&a, 62, 100, 20);
        a8(&a, 0xFF);
        start(&a, NULL);
        run_ticks(23);
        CHK("sseq_note_wait",
            ev_count(WL_SYN_EV_NOTE_ON) == 2u && ev_nth(WL_SYN_EV_NOTE_ON, 1)->e.tick == 21u);
    }
    // Loops: D4 count ... FC, nested loops, and a count of 0 that never ends (until the jump out).
    {
        Asm a = {{0}, 0};
        prog(&a, P_PSG50);
        c1(&a, 0xD4, 3);
        note(&a, 60, 100, 10);
        a8(&a, 0xFC);
        note(&a, 62, 100, 10);
        a8(&a, 0xFF);
        start(&a, NULL);
        run_ticks(33);
        CHK("sseq_loop_count", ev_count(WL_SYN_EV_NOTE_ON) == 4u &&
                                   ev_nth(WL_SYN_EV_NOTE_ON, 3)->e.a == 62 &&
                                   ev_nth(WL_SYN_EV_NOTE_ON, 2)->e.tick == 21u);
        Asm b = {{0}, 0};
        prog(&b, P_PSG50);
        c1(&b, 0xD4, 2);
        c1(&b, 0xD4, 3);
        note(&b, 60, 100, 4);
        a8(&b, 0xFC);
        note(&b, 62, 100, 4);
        a8(&b, 0xFC);
        a8(&b, 0xFF);
        start(&b, NULL);
        run_ticks(36);
        CHKF("sseq_loop_nested", ev_count(WL_SYN_EV_NOTE_ON) == 8u, "notes %u",
             ev_count(WL_SYN_EV_NOTE_ON));
        Asm c = {{0}, 0};
        prog(&c, P_PSG50);
        c1(&c, 0xD4, 0);
        note(&c, 60, 100, 10);
        a8(&c, 0xFC);
        start(&c, NULL);
        run(16000); // 500 ms: 48 ticks, notes of 10 ticks
        CHKF("sseq_loop_forever", ev_count(WL_SYN_EV_NOTE_ON) == 5u && !wl_syn_done(&g_syn),
             "notes %u", ev_count(WL_SYN_EV_NOTE_ON));
    }
    // Jump and call/return, with the call nested two deep.
    {
        Asm a = {{0}, 0};
        prog(&a, P_PSG50);
        a8(&a, 0x95); // call A
        const uint32_t fix_a = a.n;
        a24(&a, 0);
        note(&a, 65, 100, 4);
        a8(&a, 0x94); // jump over the subroutines
        const uint32_t fix_end = a.n;
        a24(&a, 0);
        const uint32_t sub_a = a.n;
        note(&a, 60, 100, 4);
        a8(&a, 0x95); // call B
        const uint32_t fix_b = a.n;
        a24(&a, 0);
        note(&a, 62, 100, 4);
        a8(&a, 0xFD);
        const uint32_t sub_b = a.n;
        note(&a, 61, 100, 4);
        a8(&a, 0xFD);
        const uint32_t end = a.n;
        a8(&a, 0xFF);
        a.b[fix_a] = (uint8_t)sub_a;
        a.b[fix_b] = (uint8_t)sub_b;
        a.b[fix_end] = (uint8_t)end;
        start(&a, NULL);
        run_ticks(20);
        const int32_t want[4] = {60, 61, 62, 65};
        bool ok = ev_count(WL_SYN_EV_NOTE_ON) == 4u;
        for (uint32_t i = 0; ok && i < 4; i++) {
            ok = ev_nth(WL_SYN_EV_NOTE_ON, i)->e.a == want[i];
        }
        CHK("sseq_call_return", ok);
    }
    // Variables: arithmetic, comparisons and the conditional prefix.
    {
        Asm a = {{0}, 0};
        varop(&a, 0xB0, 0, 5);
        varop(&a, 0xB1, 0, 3);
        varop(&a, 0xB2, 1, -4); // var1 = 0 - (-4)
        varop(&a, 0xB0, 2, 6);
        varop(&a, 0xB3, 2, 7);
        varop(&a, 0xB0, 3, 100);
        varop(&a, 0xB4, 3, 7);
        varop(&a, 0xB0, 4, 1);
        varop(&a, 0xB5, 4, 4);
        varop(&a, 0xB5, 4, -2);
        varop(&a, 0xB0, 20, -9); // a shared variable
        a8(&a, 0xFF);
        start(&a, NULL);
        run(500);
        CHKF("sseq_var_arith",
             wl_syn_var_get(&g_syn, 0) == 8 && wl_syn_var_get(&g_syn, 1) == 4 &&
                 wl_syn_var_get(&g_syn, 2) == 42 && wl_syn_var_get(&g_syn, 3) == 14 &&
                 wl_syn_var_get(&g_syn, 4) == 4 && wl_syn_var_get(&g_syn, 20) == -9,
             "%d %d %d %d %d", (int)wl_syn_var_get(&g_syn, 0), (int)wl_syn_var_get(&g_syn, 1),
             (int)wl_syn_var_get(&g_syn, 2), (int)wl_syn_var_get(&g_syn, 3),
             (int)wl_syn_var_get(&g_syn, 4));
    }
    {
        // Each comparison against 5 with a value of 5, 3 and 8: which of the three runs the
        // conditional note.
        const struct {
            uint32_t op;
            uint32_t mask; // bit i: true for values {5, 3, 8}[i]
        } cmp[] = {{0xB8, 1u}, {0xB9, 5u}, {0xBA, 4u}, {0xBB, 3u}, {0xBC, 2u}, {0xBD, 6u}};
        const int32_t vals[3] = {5, 3, 8};
        bool ok = true;
        for (uint32_t c = 0; c < 6; c++) {
            for (uint32_t k = 0; k < 3; k++) {
                Asm a = {{0}, 0};
                prog(&a, P_PSG50);
                varop(&a, 0xB0, 0, vals[k]);
                varop(&a, cmp[c].op, 0, 5);
                a8(&a, 0xA2);
                note(&a, 60, 100, 2);
                a8(&a, 0xFF);
                start(&a, NULL);
                run(300);
                ok = ok && (ev_count(WL_SYN_EV_NOTE_ON) == 1u) == (((cmp[c].mask >> k) & 1u) != 0);
            }
        }
        CHK("sseq_var_compare_and_if", ok);
        // A skipped command still consumes its operands.
        Asm a = {{0}, 0};
        prog(&a, P_PSG50);
        varop(&a, 0xB0, 0, 1);
        varop(&a, 0xB8, 0, 2); // false
        a8(&a, 0xA2);
        note(&a, 60, 100, 2000); // a two-byte length that must be skipped whole
        note(&a, 62, 100, 2);
        a8(&a, 0xFF);
        start(&a, NULL);
        run(300);
        CHK("sseq_if_skips_operands",
            ev_count(WL_SYN_EV_NOTE_ON) == 1u && ev_nth(WL_SYN_EV_NOTE_ON, 0)->e.a == 62);
    }
    // Variable operands (A1) and random operands (A0).
    {
        Asm a = {{0}, 0};
        prog(&a, P_PSG50);
        varop(&a, 0xB0, 1, 24);
        a8(&a, 0xA1);
        note(&a, 60, 100, 0); // the length byte is replaced by the variable number
        a.n--;
        a8(&a, 1);
        a8(&a, 0xA1);
        a8(&a, 0xC1); // track volume from variable 1 (24)
        a8(&a, 1);
        a8(&a, 0xFF);
        start(&a, NULL);
        run_ticks(26);
        CHK("sseq_variable_operand", ev_count(WL_SYN_EV_NOTE_ON) == 1u &&
                                         ev_nth(WL_SYN_EV_NOTE_ON, 0)->e.c == 24 &&
                                         g_syn.trk[0].vol == 24);
        Asm b = {{0}, 0};
        prog(&b, P_PSG50);
        c1(&b, 0xC7, 0);
        for (uint32_t i = 0; i < 40; i++) {
            a8(&b, 0xA0);
            a8(&b, 60);
            a8(&b, 100);
            a16(&b, 10);
            a16(&b, 20);
            rest(&b, 1);
        }
        a8(&b, 0xFF);
        WlSynParams p1;
        wl_syn_params_default(&p1);
        p1.seed = 777;
        start(&b, &p1);
        run_ticks(42);
        int32_t first[40];
        bool in_range = ev_count(WL_SYN_EV_NOTE_ON) == 40u;
        int32_t lo = 99, hi = -1;
        for (uint32_t i = 0; in_range && i < 40; i++) {
            first[i] = ev_nth(WL_SYN_EV_NOTE_ON, i)->e.c;
            in_range = first[i] >= 10 && first[i] <= 20;
            lo = first[i] < lo ? first[i] : lo;
            hi = first[i] > hi ? first[i] : hi;
        }
        CHKF("sseq_random_range", in_range && lo <= 11 && hi >= 19, "range %d..%d", (int)lo,
             (int)hi);
        start(&b, &p1);
        run_ticks(42);
        bool same = true;
        for (uint32_t i = 0; i < 40; i++) {
            same = same && ev_nth(WL_SYN_EV_NOTE_ON, i)->e.c == first[i];
        }
        CHK("sseq_random_deterministic", same);
        p1.seed = 778;
        start(&b, &p1);
        run_ticks(42);
        same = true;
        for (uint32_t i = 0; i < 40; i++) {
            same = same && ev_nth(WL_SYN_EV_NOTE_ON, i)->e.c == first[i];
        }
        CHK("sseq_random_seeded", !same);
    }
    // Shared variables are visible to every synth that is given the same array.
    {
        int16_t shared[16];
        memset(shared, 0, sizeof shared);
        Asm a = {{0}, 0};
        varop(&a, 0xB0, 17, 123);
        a8(&a, 0xFF);
        WlSynParams p;
        wl_syn_params_default(&p);
        p.global_vars = shared;
        start(&a, &p);
        run(300);
        static WlSynth other;
        WlSynSeq oq;
        uint8_t f[16 + 0x1C + 4];
        const uint8_t one[1] = {0xFF};
        const size_t fn = synfix_sseq(f, sizeof f, one, 1);
        CHK("sseq_shared_vars", wl_syn_seq_open(&oq, f, fn) == WL_OK &&
                                    wl_syn_init(&other, &oq, &g_w.bank, &p) == WL_OK &&
                                    wl_syn_var_get(&other, 17) == 123 && shared[1] == 123 &&
                                    wl_syn_var_get(&other, 1) == 0);
    }
    // Opening tracks: each runs on its own clock.
    {
        Asm a = {{0}, 0};
        a8(&a, 0xFE);
        a16(&a, 0x0007);
        a8(&a, 0x93);
        a8(&a, 1);
        const uint32_t fix1 = a.n;
        a24(&a, 0);
        a8(&a, 0x93);
        a8(&a, 2);
        const uint32_t fix2 = a.n;
        a24(&a, 0);
        prog(&a, P_PSG50);
        note(&a, 60, 100, 30);
        note(&a, 61, 100, 30);
        a8(&a, 0xFF);
        const uint32_t t1 = a.n;
        prog(&a, P_PSG50);
        rest(&a, 10);
        note(&a, 70, 100, 5);
        a8(&a, 0xFF);
        const uint32_t t2 = a.n;
        prog(&a, P_PSG50);
        rest(&a, 20);
        note(&a, 80, 100, 5);
        a8(&a, 0xFF);
        a.b[fix1] = (uint8_t)t1;
        a.b[fix2] = (uint8_t)t2;
        start(&a, NULL);
        run_ticks(64);
        bool ok = ev_count(WL_SYN_EV_NOTE_ON) == 4u && ev_count(WL_SYN_EV_TRACK_END) == 3u;
        for (uint32_t i = 0; ok && i < 4u; i++) {
            const WlSynEvent *e = &ev_nth(WL_SYN_EV_NOTE_ON, i)->e;
            ok = (e->a == 60 && e->track == 0 && e->tick == 1u) ||
                 (e->a == 61 && e->track == 0 && e->tick == 31u) ||
                 (e->a == 70 && e->track == 1 && e->tick == 11u) ||
                 (e->a == 80 && e->track == 2 && e->tick == 21u);
        }
        CHKF("sseq_open_track", ok, "notes %u ends %u", ev_count(WL_SYN_EV_NOTE_ON),
             ev_count(WL_SYN_EV_TRACK_END));
    }
    // Transpose, pitch bend and its range.
    {
        Asm a = {{0}, 0};
        c1(&a, 0xC3, 0xFE); // -2
        prog(&a, P_PSG50);
        c1(&a, 0xC7, 0);
        note(&a, 72, 100, 96);
        a8(&a, 0xFF);
        start(&a, NULL);
        run(500);
        CHK("sseq_transpose", ev_nth(WL_SYN_EV_NOTE_ON, 0)->e.a == 70);
        Asm b = {{0}, 0};
        c1(&b, 0xC4, 64); // bend +64 of 127
        c1(&b, 0xC5, 12); // an octave range: 64 * 12 / 2 = 384 = half an octave
        prog(&b, P_PSG50);
        c1(&b, 0xC7, 0);
        note(&b, 69, 100, 96);
        a8(&b, 0xFF);
        start(&b, NULL);
        run(500);
        CHKF("sseq_bend", g_syn.voice[0].step == wl_syn_step(8u * 440u, 384), "step %u want %u",
             g_syn.voice[0].step, wl_syn_step(8u * 440u, 384));
    }
    // Tie: a note on a held voice changes its key instead of starting another.
    {
        Asm a = {{0}, 0};
        prog(&a, P_PSG50);
        c1(&a, 0xC8, 1);
        note(&a, 60, 100, 24);
        note(&a, 62, 100, 24);
        note(&a, 64, 100, 24);
        c1(&a, 0xC8, 0);
        note(&a, 65, 100, 24);
        a8(&a, 0xFF);
        start(&a, NULL);
        run(26000);
        CHKF("sseq_tie",
             ev_count(WL_SYN_EV_NOTE_ON) == 2u && g_syn.stats.ties == 2u &&
                 g_syn.voice[0].key == 64,
             "notes %u ties %u", ev_count(WL_SYN_EV_NOTE_ON), g_syn.stats.ties);
    }
    // Priority and stealing: two voices only.
    {
        Asm a = {{0}, 0};
        a8(&a, 0x93);
        a8(&a, 1);
        const uint32_t fix1 = a.n;
        a24(&a, 0);
        a8(&a, 0x93);
        a8(&a, 2);
        const uint32_t fix2 = a.n;
        a24(&a, 0);
        a8(&a, 0x93);
        a8(&a, 3);
        const uint32_t fix3 = a.n;
        a24(&a, 0);
        c1(&a, 0xC6, 100);
        prog(&a, P_PSG50);
        c1(&a, 0xC7, 0);
        note(&a, 60, 100, 96);
        a8(&a, 0xFF);
        const uint32_t t1 = a.n;
        c1(&a, 0xC6, 100);
        prog(&a, P_PSG50);
        c1(&a, 0xC7, 0);
        rest(&a, 5);
        note(&a, 62, 100, 96);
        a8(&a, 0xFF);
        const uint32_t t2 = a.n;
        c1(&a, 0xC6, 10);
        prog(&a, P_PSG50);
        rest(&a, 10);
        note(&a, 64, 100, 96); // lower priority than both: dropped
        a8(&a, 0xFF);
        const uint32_t t3 = a.n;
        c1(&a, 0xC6, 200);
        prog(&a, P_PSG50);
        rest(&a, 20);
        note(&a, 66, 100, 96); // higher: steals the oldest of the two
        a8(&a, 0xFF);
        a.b[fix1] = (uint8_t)t1;
        a.b[fix2] = (uint8_t)t2;
        a.b[fix3] = (uint8_t)t3;
        WlSynParams p;
        wl_syn_params_default(&p);
        p.voice_limit = 2;
        start(&a, &p);
        run(8000);
        CHKF("sseq_steal",
             g_syn.stats.notes == 3u && g_syn.stats.notes_dropped == 1u &&
                 g_syn.stats.steals == 1u && ev_count(WL_SYN_EV_STEAL) == 1u &&
                 wl_syn_voices_active(&g_syn) == 2u,
             "notes %u dropped %u steals %u", g_syn.stats.notes, g_syn.stats.notes_dropped,
             g_syn.stats.steals);
        bool kept = false;
        for (uint32_t i = 0; i < 2; i++) {
            kept = kept || g_syn.voice[i].key == 62;
        }
        CHK("sseq_steal_keeps_newer", kept);
    }
    // A releasing voice is the first to be taken.
    {
        Asm a = {{0}, 0};
        a8(&a, 0x93);
        a8(&a, 1);
        const uint32_t fix1 = a.n;
        a24(&a, 0);
        c1(&a, 0xD3, 1); // slowest release: the voice lingers
        prog(&a, P_PSG50);
        note(&a, 60, 100, 5);
        rest(&a, 40);
        note(&a, 72, 100, 96);
        a8(&a, 0xFF);
        const uint32_t t1 = a.n;
        prog(&a, P_PSG50);
        c1(&a, 0xC7, 0);
        note(&a, 61, 100, 96);
        a8(&a, 0xFF);
        a.b[fix1] = (uint8_t)t1;
        WlSynParams p;
        wl_syn_params_default(&p);
        p.voice_limit = 2;
        start(&a, &p);
        run(16000);
        CHKF("sseq_steal_releasing_first",
             g_syn.stats.steals == 1u && g_syn.stats.notes_dropped == 0u &&
                 g_syn.voice[0].key != 60 && g_syn.voice[1].key != 60,
             "steals %u dropped %u", g_syn.stats.steals, g_syn.stats.notes_dropped);
    }
    // End of the sequence: tracks end, the voices die, the output is silent and done is set.
    {
        Asm a = {{0}, 0};
        prog(&a, P_PSG50);
        note(&a, 60, 100, 10);
        a8(&a, 0xFF);
        start(&a, NULL);
        run(8000);
        bool quiet = true;
        for (uint32_t i = 6000; i < 8000u; i++) {
            quiet = quiet && g_out[2u * i] == 0 && g_out[2u * i + 1u] == 0;
        }
        CHK("sseq_ends", wl_syn_done(&g_syn) && wl_syn_voices_active(&g_syn) == 0 && quiet &&
                             wl_syn_tracks_active(&g_syn) == 0);
    }
    // Defective sequences end the track without harm.
    {
        Asm a = {{0}, 0};
        prog(&a, P_PSG50);
        a8(&a, 0x82); // not a command of this player
        note(&a, 60, 100, 10);
        start(&a, NULL);
        run(500);
        CHKF("sseq_unknown_command",
             g_syn.stats.unknown_cmds == 1u && g_syn.stats.last_unknown == 0x82 &&
                 ev_count(WL_SYN_EV_NOTE_ON) == 0u && wl_syn_tracks_active(&g_syn) == 0,
             "unknown %u", g_syn.stats.unknown_cmds);
        Asm b = {{0}, 0};
        a8(&b, 0x3C); // a note cut short
        start(&b, NULL);
        run(500);
        CHK("sseq_truncated",
            wl_syn_tracks_active(&g_syn) == 0 && ev_count(WL_SYN_EV_NOTE_ON) == 0u);
        Asm c = {{0}, 0};
        a8(&c, 0x94);
        a24(&c, 0xFFFFF); // a jump past the end
        start(&c, NULL);
        run(500);
        CHK("sseq_jump_range", wl_syn_tracks_active(&g_syn) == 0);
        Asm d = {{0}, 0};
        a8(&d, 0x94);
        a24(&d, 0); // a loop that never waits
        start(&d, NULL);
        run(2000);
        CHKF("sseq_runaway", g_syn.stats.runaway > 3u && wl_syn_tracks_active(&g_syn) == 1u,
             "runaway %u", g_syn.stats.runaway);
        Asm e = {{0}, 0};
        c1(&e, 0xD4, 2);
        c1(&e, 0xD4, 2);
        c1(&e, 0xD4, 2);
        c1(&e, 0xD4, 2); // deeper than the stack: the fourth is ignored, the rest still runs
        a8(&e, 0xFF);
        start(&e, NULL);
        run(300);
        CHK("sseq_stack_depth",
            g_syn.stats.unknown_cmds == 0u && wl_syn_tracks_active(&g_syn) == 0);
        CHK("sseq_init_empty", wl_syn_init(&g_syn, &g_seq, &g_w.bank, NULL) == WL_OK &&
                                   wl_syn_init(NULL, &g_seq, &g_w.bank, NULL) != WL_OK);
        WlSynSeq empty;
        memset(&empty, 0, sizeof empty);
        CHK("sseq_init_args", wl_syn_init(&g_syn, &empty, &g_w.bank, NULL) != WL_OK);
    }
    // A note with no instrument or no wave is counted, not played.
    {
        Asm a = {{0}, 0};
        prog(&a, P_EMPTY);
        note(&a, 60, 100, 10);
        prog(&a, 50);
        note(&a, 60, 100, 10);
        a8(&a, 0xFF);
        start(&a, NULL);
        run(4000);
        CHK("sseq_silent_notes", g_syn.stats.notes_silent == 2u && g_syn.stats.notes == 0u);
        WlSynBank nowaves = g_w.bank;
        wl_syn_bank_set_waves(&nowaves, 0, NULL);
        Asm b = {{0}, 0};
        prog(&b, P_PCM16);
        note(&b, 60, 100, 10);
        a8(&b, 0xFF);
        const size_t n = synfix_sseq(g_sseq, sizeof g_sseq, b.b, b.n);
        WlSynParams p;
        wl_syn_params_default(&p);
        CHK("sseq_missing_wave", wl_syn_seq_open(&g_seq, g_sseq, n) == WL_OK &&
                                     wl_syn_init(&g_syn, &g_seq, &nowaves, &p) == WL_OK &&
                                     (run(800), true) && g_syn.stats.notes_silent == 1u);
    }
}

// ---- modulation, sweep and portamento ----------------------------------------------------------

static void t_modulation(void) {
    const uint32_t base = wl_syn_step(8u * 440u, 0);
    // Vibrato: depth 127, range 1, speed 16: +-63/64 semitone, a cycle every 32 frames; E0 delays
    // it.
    Asm a = {{0}, 0};
    c1(&a, 0xCA, 127);
    c1(&a, 0xCD, 1);
    a8(&a, 0xE0);
    a16(&a, 60);
    prog(&a, P_PSG50);
    note(&a, 69, 100, 96);
    a8(&a, 0xFF);
    start(&a, NULL);
    uint32_t lo = 1u << 30, hi = 0, steady = 0;
    for (uint32_t f = 1; f <= 200u; f++) {
        run_to_frame(f);
        const uint32_t st = g_syn.voice[0].step;
        if (f <= 60u) {
            steady += st == base ? 1u : 0u;
        } else {
            lo = st < lo ? st : lo;
            hi = st > hi ? st : hi;
        }
    }
    const uint32_t want_hi = wl_syn_step(8u * 440u, 63);
    const uint32_t want_lo = wl_syn_step(8u * 440u, -63);
    CHKF("sseq_lfo_delay", steady == 60u, "steady frames %u", steady);
    CHKF("sseq_lfo_pitch_range",
         hi >= want_hi - want_hi / 100u && hi <= want_hi + want_hi / 100u && lo >= want_lo - 80u &&
             lo <= want_lo + want_lo / 100u,
         "step %u..%u want %u..%u", lo, hi, want_lo, want_hi);
    // Volume and pan modulation move the gains.
    for (uint32_t type = 1; type <= 2; type++) {
        Asm b = {{0}, 0};
        c1(&b, 0xCA, 127);
        c1(&b, 0xCC, type);
        c1(&b, 0xCD, type == 1 ? 8 : 1);
        prog(&b, P_PSG50);
        note(&b, 69, 100, 96);
        a8(&b, 0xFF);
        start(&b, NULL);
        int32_t lmin = 1 << 30, lmax = 0, rmin = 1 << 30, rmax = 0;
        for (uint32_t f = 10; f <= 120u; f++) {
            run_to_frame(f);
            lmin = g_syn.voice[0].gl < lmin ? g_syn.voice[0].gl : lmin;
            lmax = g_syn.voice[0].gl > lmax ? g_syn.voice[0].gl : lmax;
            rmin = g_syn.voice[0].gr < rmin ? g_syn.voice[0].gr : rmin;
            rmax = g_syn.voice[0].gr > rmax ? g_syn.voice[0].gr : rmax;
        }
        char nm[40];
        (void)snprintf(nm, sizeof nm, "sseq_lfo_%s", type == 1 ? "volume" : "pan");
        CHKF(nm,
             (type == 1 ? (lmax > lmin + lmin / 4 && rmax > rmin + rmin / 4)
                        : (lmax - lmin > (1 << 17) && rmax - rmin > (1 << 17))),
             "L %d..%d R %d..%d", (int)lmin, (int)lmax, (int)rmin, (int)rmax);
    }
    // Portamento from key 57 to 69 over the note's length (96 ticks = 192 frames): starts an octave
    // down.
    Asm c = {{0}, 0};
    c1(&c, 0xC9, 57);
    prog(&c, P_PSG50);
    note(&c, 69, 100, 96);
    a8(&c, 0xFF);
    start(&c, NULL);
    run_to_frame(2);
    const uint32_t s2 = g_syn.voice[0].step;
    run_to_frame(96);
    const uint32_t s96 = g_syn.voice[0].step;
    run_to_frame(191);
    const uint32_t s190 = g_syn.voice[0].step;
    CHKF("sseq_porta_start",
         s2 < wl_syn_step(8u * 440u, -(12 * 64) + 16) &&
             s2 > wl_syn_step(8u * 440u, -(12 * 64) - 16),
         "step %u", s2);
    CHKF("sseq_porta_middle",
         s96 > wl_syn_step(8u * 440u, -(6 * 64) - 24) &&
             s96 < wl_syn_step(8u * 440u, -(6 * 64) + 24),
         "step %u", s96);
    CHKF("sseq_porta_end", s190 > wl_syn_step(8u * 440u, -16) && s190 <= base, "step %u", s190);
    // Portamento with a time: a quadratic length in frames.
    Asm d = {{0}, 0};
    c1(&d, 0xC9, 57);
    c1(&d, 0xCF, 16); // 16 * 16 * 768 >> 11 = 96 frames
    prog(&d, P_PSG50);
    note(&d, 69, 100, 96);
    a8(&d, 0xFF);
    start(&d, NULL);
    run_to_frame(49);
    const uint32_t t49 = g_syn.voice[0].step;
    run_to_frame(100);
    const uint32_t t100 = g_syn.voice[0].step;
    CHKF("sseq_porta_time",
         t49 > wl_syn_step(8u * 440u, -(6 * 64) - 24) &&
             t49 < wl_syn_step(8u * 440u, -(6 * 64) + 24) && t100 == base,
         "steps %u %u", t49, t100);
    // The sweep command: a starting offset that dies away over the note.
    Asm e = {{0}, 0};
    a8(&e, 0xE3);
    a16(&e, 384);
    prog(&e, P_PSG50);
    note(&e, 69, 100, 96);
    a8(&e, 0xFF);
    start(&e, NULL);
    run_to_frame(2);
    const uint32_t w2 = g_syn.voice[0].step;
    run_to_frame(96);
    const uint32_t w96 = g_syn.voice[0].step;
    CHKF("sseq_sweep",
         w2 > wl_syn_step(8u * 440u, 384 - 16) && w96 > wl_syn_step(8u * 440u, 192 - 24) &&
             w96 < wl_syn_step(8u * 440u, 192 + 24),
         "steps %u %u", w2, w96);
}

// ---- a rendered piece with a digest that must be the same everywhere ----------------------------

// Three tracks: an ADPCM lead with vibrato and a bend, a looping PCM8 pad with a slow attack, and
// PSG and noise percussion with the drum set, over about 3 seconds.
static void build_piece(Asm *a) {
    a8(a, 0xFE);
    a16(a, 0x0007);
    a8(a, 0x93);
    a8(a, 1);
    const uint32_t f1 = a->n;
    a24(a, 0);
    a8(a, 0x93);
    a8(a, 2);
    const uint32_t f2 = a->n;
    a24(a, 0);
    a8(a, 0xE1);
    a16(a, 150);
    // Track 0: the lead.
    c1(a, 0xC0, 40);
    c1(a, 0xC1, 110);
    prog(a, P_ADPCM);
    c1(a, 0xCA, 30);
    c1(a, 0xCB, 20);
    c1(a, 0xD3, 90);
    c1(a, 0xC8, 0);
    c1(a, 0xD4, 4);
    note(a, 60, 110, 24);
    note(a, 64, 100, 12);
    c1(a, 0xC4, 20);
    note(a, 67, 100, 24);
    c1(a, 0xC4, 0);
    note(a, 72, 90, 36);
    rest(a, 12);
    a8(a, 0xFC);
    a8(a, 0xFF);
    // Track 1: the pad.
    const uint32_t t1 = a->n;
    c1(a, 0xC0, 90);
    prog(a, P_SPLIT);
    c1(a, 0xC7, 0);
    c1(a, 0xD0, 70);
    c1(a, 0xD3, 60);
    c1(a, 0xC1, 90);
    c1(a, 0xD4, 2);
    note(a, 48, 90, 96);
    note(a, 55, 90, 96);
    note(a, 64, 80, 96);
    rest(a, 96);
    a8(a, 0xFC);
    a8(a, 0xFF);
    // Track 2: drums and a noise burst.
    const uint32_t t2 = a->n;
    prog(a, P_DRUMS);
    c1(a, 0xD4, 8);
    note(a, 36, 120, 24);
    note(a, 39, 100, 12);
    note(a, 37, 100, 12);
    a8(a, 0xFC);
    c1(a, 0xC0, 90);
    prog(a, P_NOISE);
    note(a, 72, 60, 6);
    a8(a, 0xFF);
    a->b[f1] = (uint8_t)t1;
    a->b[f1 + 1] = (uint8_t)(t1 >> 8);
    a->b[f2] = (uint8_t)t2;
    a->b[f2 + 1] = (uint8_t)(t2 >> 8);
}

#define PIECE_DIGEST 0x9C73CE38u

static void t_digest(void) {
    Asm a = {{0}, 0};
    build_piece(&a);
    CHK("sseq_piece_start", start(&a, NULL));
    run(PIECE_FRAMES);
    const uint32_t h = fnv(g_out, 2u * PIECE_FRAMES);
    wlt_report("INFO sseq_piece digest=%08X voices_peak=%u notes=%u clipped=%u", (unsigned)h,
               g_syn.stats.voices_peak, g_syn.stats.notes, g_syn.stats.clipped);
    CHKF("sseq_piece_active",
         g_syn.stats.notes >= 25u && g_syn.stats.voices_peak >= 5u &&
             g_syn.stats.unknown_cmds == 0u,
         "notes %u peak %u", g_syn.stats.notes, g_syn.stats.voices_peak);
    CHKF("sseq_piece_digest", h == PIECE_DIGEST, "digest %08X want %08X", (unsigned)h,
         (unsigned)PIECE_DIGEST);
    // Chunking does not matter: rendering in odd sizes gives the same samples.
    start(&a, NULL);
    uint32_t done = 0, k = 0;
    static int16_t again[2u * 24000u];
    while (done < 24000u) {
        uint32_t n = 1u + (k++ * 37u) % 300u;
        if (n > 24000u - done) {
            n = 24000u - done;
        }
        wl_syn_render(&g_syn, again + 2u * done, n);
        done += n;
    }
    CHK("sseq_chunk_independent", memcmp(again, g_out, sizeof again) == 0);
    // Paused output is silent and does not advance the sequence.
    start(&a, NULL);
    wl_syn_set_paused(&g_syn, true);
    run(2000);
    bool quiet = true;
    for (uint32_t i = 0; i < 4000u; i++) {
        quiet = quiet && g_out[i] == 0;
    }
    const uint32_t ticks_paused = g_syn.stats.ticks;
    wl_syn_set_paused(&g_syn, false);
    run(2000);
    CHK("sseq_pause", quiet && ticks_paused == 0u && g_syn.stats.ticks > 0u);
}

void wlt_sseq_core(void) {
    g_world_ok = build_world();
    t_tables();
    t_parsers();
    if (!g_world_ok) {
        return;
    }
    t_wave_exact();
    t_envelope();
    t_psg();
    t_timing();
    t_commands();
    t_modulation();
    t_digest();
}

// ---- cost and the engine path (the host stub; not the big-endian test)
// -------------

#ifndef WL_SSEQ_CORE_ONLY

// Four tracks of eight sustained notes each: 32 voices at once.
static void build_dense(Asm *a) {
    a8(a, 0x93);
    a8(a, 1);
    const uint32_t f1 = a->n;
    a24(a, 0);
    a8(a, 0x93);
    a8(a, 2);
    const uint32_t f2 = a->n;
    a24(a, 0);
    a8(a, 0x93);
    a8(a, 3);
    const uint32_t f3 = a->n;
    a24(a, 0);
    uint32_t starts[4];
    const uint32_t progs[4] = {P_ADPCM, P_PCM16, P_PCM8, P_PSG50};
    for (uint32_t t = 0; t < 4; t++) {
        starts[t] = a->n;
        c1(a, 0xC0, 20u + 30u * t);
        prog(a, progs[t]);
        c1(a, 0xC7, 0);
        c1(a, 0xCA, 20);
        c1(a, 0xD3, 100);
        for (uint32_t i = 0; i < 8; i++) {
            note(a, 48u + 3u * i + t, 70, 190);
        }
        rest(a, 190);
        a8(a, 0xFF);
    }
    a->b[f1] = (uint8_t)starts[1];
    a->b[f1 + 1] = (uint8_t)(starts[1] >> 8);
    a->b[f2] = (uint8_t)starts[2];
    a->b[f2 + 1] = (uint8_t)(starts[2] >> 8);
    a->b[f3] = (uint8_t)starts[3];
    a->b[f3 + 1] = (uint8_t)(starts[3] >> 8);
}

static void t_cost(void) {
    Asm a = {{0}, 0};
    build_dense(&a);
    if (!start(&a, NULL)) {
        wlt_check("sseq_cost_start", false, "start");
        return;
    }
    // Warm up until all voices sound, then time 288-frame blocks (the hardware block of the audio
    // engine).
    run(4000);
    const uint32_t active = wl_syn_voices_active(&g_syn);
    const uint32_t blocks = 400;
    uint64_t total = 0, worst = 0;
    static int16_t blk[2 * 288];
    const uint64_t vs0 = g_syn.stats.voice_samples;
    for (uint32_t b = 0; b < blocks; b++) {
        const uint64_t t0 = wl_platform_fs_clock_us();
        wl_syn_render(&g_syn, blk, 288);
        const uint64_t dt = wl_platform_fs_clock_us() - t0;
        total += dt;
        worst = dt > worst ? dt : worst;
    }
    const uint64_t vs = g_syn.stats.voice_samples - vs0;
    const uint32_t mean = (uint32_t)(total / blocks);
    const uint32_t per_voice_x100 =
        vs != 0 ? (uint32_t)(total * 28800u / vs) : 0; // us per 288-frame block per voice x100
    wlt_report(
        "INFO sseq_cost voices=%u blocks=%u mean_us=%u max_us=%u per_voice_block_us_x100=%u "
        "voice_samples_per_block=%u",
        active, blocks, mean, (unsigned)worst, per_voice_x100, (unsigned)(vs / blocks));
    CHKF("sseq_cost_voices", active >= 28u, "active voices %u", active);
    // The budget: 1.5 ms of CPU per 16.7 ms frame is 810 us per 9 ms block at 32 voices. This is a
    // smoke limit (twice the budget); the measured numbers are the result.
    CHKF("sseq_cost_budget", mean < 1620u, "mean %u us per 288-frame block", mean);
}

static void t_engine(void) {
    if (!wl_mem_ready() && wl_mem_init(NULL) != WL_OK) {
        wlt_check("sseq_engine_mem", false, "wl_mem_init");
        return;
    }
    const WlResult r = wl_audio_init();
    wlt_check("sseq_engine_init", r == WL_OK, "wl_audio_init");
    if (r != WL_OK) {
        return;
    }
    WlAudioSong song;
    memset(&song, 0, sizeof song);
    Asm a = {{0}, 0};
    build_piece(&a);
    static uint8_t seqf[2048];
    song.seq = seqf;
    song.seq_size = synfix_sseq(seqf, sizeof seqf, a.b, a.n);
    song.bank = g_w.sbnk;
    song.bank_size = g_w.sbnk_n;
    song.waves[0] = g_w.swar;
    song.wave_size[0] = g_w.swar_n;
    WlAudioHandle h = 0;
    WlResult pr = wl_audio_seq_play(&song, NULL, NULL, &h);
    CHKF("sseq_engine_play", pr == WL_OK && h != 0, "result %d", (int)pr);
    WlAudioHandle h2 = 0;
    CHK("sseq_engine_bad_song", wl_audio_seq_play(&(WlAudioSong){0}, NULL, NULL, &h2) != WL_OK);
    song.bank_size = 12;
    CHK("sseq_engine_bad_bank", wl_audio_seq_play(&song, NULL, NULL, &h2) == WL_ERR_FORMAT);
    song.bank_size = g_w.sbnk_n;
    for (uint32_t i = 0; i < 200; i++) {
        wl_platform_thread_sleep_ms(10);
        wl_audio_pump(NULL, NULL);
    }
    WlSynStats st;
    CHK("sseq_engine_stats", wl_audio_seq_stats(h, &st) && st.notes > 3u && st.frames > 100u);
    CHK("sseq_engine_playing", wl_audio_state(h) == WL_AUDIO_PLAYING);
    wl_audio_seq_pause(h, true);
    wl_audio_stop(h, 50);
    for (uint32_t i = 0; i < 60 && wl_audio_state(h) != WL_AUDIO_GONE; i++) {
        wl_platform_thread_sleep_ms(10);
        wl_audio_pump(NULL, NULL);
    }
    CHK("sseq_engine_stop", wl_audio_state(h) == WL_AUDIO_GONE);
    WlAudioStats as;
    wl_audio_stats(&as);
    wl_audio_log_stats("sseq");
    CHKF("sseq_engine_no_underruns", as.underruns == 0u, "underruns %u", as.underruns);
    wl_audio_shutdown();
}

void wlt_suite_sseq(void) {
    wlt_sseq_core();
    t_cost();
#ifndef WL_SSEQ_NO_ENGINE
    if (g_world_ok) {
        t_engine();
    }
#endif
}

#endif
