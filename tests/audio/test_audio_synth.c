// SPDX-License-Identifier: MIT
// The SSEQ synth (wl/audio/synth*.c) on the host: the `sseq` suite's
// deterministic checks (suite_sseq.c: tables, parsers, every command, instruments,
// envelopes, pitch, loops, IMA-ADPCM exactness, stealing, the digest of a rendered piece), and the
// synth inside the audio engine (wl_audio_seq_play on the host stub, where this test is the audio
// thread).
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wl/audio/audio.h"
#include "wl/platform/host/audio_host.h"
#include "wl/platform/io.h"
#include "wl/platform/mem.h"
#include "tests/audio/synth_fixture.h"
#include "tests/audio/check.h"
#include "greatest.h"

static int g_pass, g_fail;

void wlt_report(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void wlt_check(const char *name, bool ok, const char *detail) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL %s: %s\n", name, detail ? detail : "-");
    }
}

void wlt_sseq_core(void);

TEST sseq_core_checks(void) {
    wlt_sseq_core();
    printf("sseq core: %d checks passed, %d failed\n", g_pass, g_fail);
    ASSERT(g_pass >= 140);
    ASSERT_EQ(0, g_fail);
    PASS();
}

// ---- the synth as a mixer voice ---------------------------------------------------------------

typedef struct {
    uint8_t seq[256], bank[256], war[256];
    size_t seq_n, bank_n, war_n;
} Song;

// A short song on a PSG square and a looping PCM16 wave: 4 notes of 12 ticks, then the end.
static void make_song(Song *s, uint32_t notes, bool loop_forever) {
    uint8_t cmds[64];
    uint32_t n = 0;
    cmds[n++] = 0xC0; // hard left
    cmds[n++] = 0;
    cmds[n++] = 0x81;
    cmds[n++] = 0;
    uint32_t top = n;
    for (uint32_t i = 0; i < notes; i++) {
        cmds[n++] = (uint8_t)(60 + 2 * i);
        cmds[n++] = 100;
        cmds[n++] = 12;
    }
    if (loop_forever) {
        cmds[n++] = 0x94;
        cmds[n++] = (uint8_t)top;
        cmds[n++] = 0;
        cmds[n++] = 0;
    } else {
        cmds[n++] = 0xFF;
    }
    s->seq_n = synfix_sseq(s->seq, sizeof s->seq, cmds, n);
    SynfixInst in[2];
    memset(in, 0, sizeof in);
    in[0] = synfix_single(2, 3, 60, 127, 127, 127, 127);
    in[1] = synfix_single(1, 0, 60, 127, 127, 127, 127);
    s->bank_n = synfix_sbnk(s->bank, sizeof s->bank, in, 2);
    s->war_n = 0;
}

static WlAudioSong as_audio_song(const Song *s) {
    WlAudioSong a;
    memset(&a, 0, sizeof a);
    a.seq = s->seq;
    a.seq_size = s->seq_n;
    a.bank = s->bank;
    a.bank_size = s->bank_n;
    return a;
}

static void engine_up(void) {
    wl_host_audio_thread_stop();
    wl_audio_shutdown();
    (void)wl_mem_shutdown();
    const WlResult rm = wl_mem_init(NULL);
    const WlResult ra = rm == WL_OK ? wl_audio_init() : rm;
    if (ra != WL_OK) {
        fprintf(stderr, "engine did not start (%d)\n", (int)ra);
        exit(2);
    }
    wl_audio_bus_volume(WL_BUS_MUSIC, 256); // unity: exact comparisons
    wl_audio_sync();
    wl_host_audio_render(NULL, 256);
}

static void render_blocks(int16_t *out, uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; b++) {
        wl_host_audio_render(out + 512u * b, 256);
        wl_audio_pump(NULL, NULL);
    }
}

TEST engine_plays_the_synth_exactly(void) {
    engine_up();
    static Song song;
    make_song(&song, 4, false);
    const WlAudioSong as = as_audio_song(&song);
    WlAudioHandle h = 0;
    ASSERT_EQ(WL_OK, wl_audio_seq_play(&as, NULL, NULL, &h));
    ASSERT(h != 0);
    // The reference: the same sequence on a private synth, rendered in the same sizes.
    static WlSynth ref;
    static WlSynSeq rseq;
    static WlSynBank rbank;
    ASSERT_EQ(WL_OK, wl_syn_seq_open(&rseq, song.seq, song.seq_n));
    ASSERT_EQ(WL_OK, wl_syn_bank_open(&rbank, song.bank, song.bank_n));
    ASSERT_EQ(WL_OK, wl_syn_init(&ref, &rseq, &rbank, NULL));
    static int16_t eng[512 * 40], want[512 * 40];
    render_blocks(eng, 40);
    wl_syn_render(&ref, want, 256u * 40u);
    int differ = -1;
    for (uint32_t i = 0; i < 2u * 256u * 40u; i++) {
        if (eng[i] != want[i]) {
            differ = (int)i;
            break;
        }
    }
    if (differ >= 0) {
        printf("first difference at sample %d: engine %d, synth %d\n", differ, eng[differ],
               want[differ]);
    }
    ASSERT_EQ(-1, differ);
    bool loud = false;
    for (uint32_t i = 0; i < 2u * 256u * 40u; i++) {
        loud = loud || eng[i] > 2000;
    }
    ASSERT(loud);
    // 4 notes of 12 ticks end at tick 49 (0.5 s); the voice leaves the mixer and the slot is freed.
    ASSERT_EQ(WL_AUDIO_PLAYING,
              wl_audio_state(h) == WL_AUDIO_GONE ? WL_AUDIO_GONE : WL_AUDIO_PLAYING);
    render_blocks(eng, 40);
    wl_audio_sync();
    ASSERT_EQ(WL_AUDIO_GONE, wl_audio_state(h));
    wl_audio_shutdown();
    PASS();
}

TEST engine_pause_stop_limits_and_errors(void) {
    engine_up();
    static Song song;
    make_song(&song, 4, true); // loops forever
    const WlAudioSong as = as_audio_song(&song);
    WlAudioHandle h[WL_AUDIO_MAX_SEQS + 1];
    for (uint32_t i = 0; i < WL_AUDIO_MAX_SEQS; i++) {
        ASSERT_EQ(WL_OK, wl_audio_seq_play(&as, NULL, NULL, &h[i]));
    }
    ASSERT_EQ(WL_ERR_LIMIT, wl_audio_seq_play(&as, NULL, NULL, &h[WL_AUDIO_MAX_SEQS]));
    static int16_t blk[512 * 8];
    render_blocks(blk, 8);
    WlSynStats st;
    ASSERT(wl_audio_seq_stats(h[0], &st));
    ASSERT(st.notes >= 1 && st.frames > 5);
    ASSERT(!wl_audio_seq_stats(9999, &st));
    // Pause one: its output stops, the others go on; resume.
    for (uint32_t i = 1; i < WL_AUDIO_MAX_SEQS; i++) {
        wl_audio_stop(h[i], 0);
    }
    render_blocks(blk, 4);
    wl_audio_seq_pause(h[0], true);
    render_blocks(blk, 4); // the pause starts in the first of these
    render_blocks(blk, 4);
    bool quiet = true;
    for (uint32_t i = 0; i < 2u * 256u * 4u; i++) {
        quiet = quiet && blk[i] == 0;
    }
    ASSERT(quiet);
    WlSynStats st2;
    ASSERT(wl_audio_seq_stats(h[0], &st2));
    wl_audio_seq_pause(h[0], false);
    render_blocks(blk, 8);
    ASSERT(wl_audio_seq_stats(h[0], &st));
    ASSERT(st.frames > st2.frames);
    bool loud = false;
    for (uint32_t i = 0; i < 2u * 256u * 8u; i++) {
        loud = loud || blk[i] > 1000;
    }
    ASSERT(loud);
    // Stopped sequences give their slots back to the pump.
    render_blocks(blk, 4);
    WlAudioHandle again = 0;
    ASSERT_EQ(WL_OK, wl_audio_seq_play(&as, NULL, NULL, &again));
    // A fade-out stop ends the voice after the fade.
    wl_audio_stop(h[0], 20);
    render_blocks(blk, 8);
    wl_audio_sync();
    ASSERT_EQ(WL_AUDIO_GONE, wl_audio_state(h[0]));
    // Bad songs.
    WlAudioHandle bad = 0;
    WlAudioSong junk = as;
    junk.seq_size = 8;
    ASSERT_EQ(WL_ERR_FORMAT, wl_audio_seq_play(&junk, NULL, NULL, &bad));
    junk = as;
    junk.bank = NULL;
    ASSERT_EQ(WL_ERR_FORMAT, wl_audio_seq_play(&junk, NULL, NULL, &bad));
    junk = as;
    junk.waves[2] = song.seq; // not a wave archive
    junk.wave_size[2] = song.seq_n;
    ASSERT_EQ(WL_ERR_FORMAT, wl_audio_seq_play(&junk, NULL, NULL, &bad));
    ASSERT_EQ(WL_ERR_ARG, wl_audio_seq_play(NULL, NULL, NULL, &bad));
    ASSERT_EQ(WL_ERR_ARG, wl_audio_seq_play(&as, NULL, NULL, NULL));
    wl_audio_shutdown();
    ASSERT_EQ(WL_ERR_STATE, wl_audio_seq_play(&as, NULL, NULL, &bad));
    PASS();
}

TEST sequence_bus_and_volume(void) {
    engine_up();
    static Song song;
    make_song(&song, 4, true);
    const WlAudioSong as = as_audio_song(&song);
    static int16_t a[512 * 20], b[512 * 20];
    WlAudioHandle h = 0;
    ASSERT_EQ(WL_OK, wl_audio_seq_play(&as, NULL, NULL, &h));
    render_blocks(a, 20);
    wl_audio_shutdown();
    // The same song at half gain on the effects bus is half as loud.
    engine_up();
    WlAudioPlay play = wl_audio_play_defaults(WL_BUS_EFFECTS);
    play.gain = 128;
    ASSERT_EQ(WL_OK, wl_audio_seq_play(&as, NULL, &play, &h));
    render_blocks(b, 20);
    int32_t pa = 0, pb = 0;
    for (uint32_t i = 2u * 256u * 5u; i < 2u * 256u * 20u; i++) {
        pa = a[i] > pa ? a[i] : pa;
        pb = b[i] > pb ? b[i] : pb;
    }
    ASSERT(pa > 4000);
    ASSERT(pb > pa / 2 - 40 && pb < pa / 2 + 40);
    wl_audio_shutdown();
    PASS();
}

SUITE(audio_synth) {
    RUN_TEST(sseq_core_checks);
    RUN_TEST(engine_plays_the_synth_exactly);
    RUN_TEST(engine_pause_stop_limits_and_errors);
    RUN_TEST(sequence_bus_and_volume);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(audio_synth);
    GREATEST_MAIN_END();
}
