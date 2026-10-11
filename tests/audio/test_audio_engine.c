// SPDX-License-Identifier: MIT
// The audio engine on the host stub (wl/audio/audio.c, stream.c, fmt.c): PWST streams read from
// a file on the (temp-dir) SD card through the IO thread, loops including mid-frame ones, starvation
// and recovery, fades and crossfades, bad inputs, PWSF samples, commands, and a threaded race run.
// The test is the audio thread: it renders blocks with wl_host_audio_render.
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wl/audio/audio.h"
#include "wl/platform/fs.h"
#include "wl/platform/host/audio_host.h"
#include "wl/platform/host/fs_host.h"
#include "wl/platform/io.h"
#include "wl/platform/mem.h"
#include "tests/audio/audio_synth.h"
#include "wl/base/endian.h"
#include "tests/audio/dsp_fixture.h"
#include "greatest.h"

static char g_sd[256];

// ---- helpers ----------------------------------------------------------------------------------

static void write_file(const char *path, const uint8_t *data, size_t n) {
    WlPlatformFile *f;
    if (wl_platform_fs_open(path, WL_FILE_WRITE_NEW, &f) != WL_OK) {
        fprintf(stderr, "cannot create %s\n", path);
        exit(2);
    }
    for (size_t off = 0; off < n;) {
        uint32_t chunk = n - off > 65536 ? 65536u : (uint32_t)(n - off);
        if (wl_platform_fs_write(f, data + off, chunk) != WL_OK) {
            exit(2);
        }
        off += chunk;
    }
    wl_platform_fs_close(f);
}

static void engine_up(void) {
    wl_host_audio_thread_stop();
    wl_audio_shutdown(); // a test that failed an assertion may have left the engine running
    wl_fs_stop();
    (void)wl_mem_shutdown();
    if (wl_mem_init(NULL) != WL_OK) {
        fprintf(stderr, "wl_mem_init failed\n");
        exit(2);
    }
}

static WlResult engine_start(void) {
    wl_host_fs_set_root(WL_DEVICE_SD, g_sd);
    WlResult r = wl_platform_fs_mount(WL_DEVICE_SD);
    if (r != WL_OK) {
        return r;
    }
    r = wl_fs_start();
    if (r != WL_OK) {
        return r;
    }
    r = wl_audio_init();
    if (r != WL_OK) {
        return r;
    }
    wl_audio_bus_volume(WL_BUS_MUSIC, 256); // unity: exact comparisons
    wl_audio_sync();
    wl_host_audio_render(NULL, 256); // the bus gain ramps over this block
    return WL_OK;
}

static void engine_stop(void) {
    wl_audio_shutdown();
    wl_fs_stop();
}

#define SETUP()                                                                                    \
    do {                                                                                           \
        engine_up();                                                                               \
        ASSERT_EQ(WL_OK, engine_start());                                                          \
    } while (0)

#define TEARDOWN()                                                                                 \
    do {                                                                                           \
        engine_stop();                                                                             \
        ASSERT_EQ(0u, wl_mem_live_blocks(WL_TAG_AUDIO));                                           \
        ASSERT_EQ(0u, wl_mem_live_blocks(WL_TAG_AUDIO_BANK));                                      \
    } while (0)

// Collects reads until the IO queue is empty and the state machines have caught up.
static void settle(void) {
    for (int i = 0; i < 4000; i++) {
        wl_audio_pump(NULL, NULL);
        if (wl_io_outstanding() == 0) {
            break;
        }
        wl_platform_thread_sleep_ms(1);
    }
    wl_audio_pump(NULL, NULL);
}

static void run_blocks(int16_t *out, uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; b++) {
        settle();
        wl_host_audio_render(out != NULL ? out + (size_t)b * 512u : NULL, 256);
    }
}

static bool wait_state(WlAudioHandle h, WlAudioState want) {
    for (int i = 0; i < 4000; i++) {
        wl_audio_pump(NULL, NULL);
        if (wl_audio_state(h) == want) {
            return true;
        }
        wl_platform_thread_sleep_ms(1);
    }
    return false;
}

typedef struct {
    uint8_t *file;
    size_t size;
    int16_t *ref[2];
    FxAStreamSpec spec;
    WlAudioSpan span;
    char path[64];
} Fix;

#define FIX_PREFIX 256u // bytes of a non-PWST header in front of the stream: spans have an offset

// Builds the stream and writes it to the SD card behind FIX_PREFIX bytes of filler (as an entry
// of an uncompressed archive would sit), then points the span at it.
static WlResult fix_make(Fix *f, const FxAStreamSpec *sp, const char *file_name) {
    memset(f, 0, sizeof *f);
    f->spec = *sp;
    uint8_t *pwst;
    size_t pwst_size;
    WlResult r = fxa_build_pwst(sp, &pwst, &pwst_size, f->ref);
    if (r != WL_OK) {
        return r;
    }
    f->size = FIX_PREFIX + pwst_size;
    f->file = malloc(f->size);
    if (f->file == NULL) {
        free(pwst);
        return WL_ERR_NOMEM;
    }
    memset(f->file, 0xA5, FIX_PREFIX);
    memcpy(f->file + FIX_PREFIX, pwst, pwst_size);
    free(pwst);
    snprintf(f->path, sizeof f->path, "sd:/%s", file_name);
    write_file(f->path, f->file, f->size);
    f->span.path = f->path;
    f->span.offset = FIX_PREFIX;
    f->span.size = (uint32_t)pwst_size;
    return WL_OK;
}

static void fix_free(Fix *f) {
    free(f->file);
    free(f->ref[0]);
    free(f->ref[1]);
    memset(f, 0, sizeof *f);
}

static uint32_t pos_at(const FxAStreamSpec *sp, uint64_t i) {
    if (!sp->loop || i <= sp->loop_end) {
        return (uint32_t)i;
    }
    const uint32_t len = sp->loop_end - sp->loop_start + 1u;
    return sp->loop_start + (uint32_t)((i - sp->loop_end - 1u) % len);
}

static WlAudioPlay music(void) {
    return wl_audio_play_defaults(WL_BUS_MUSIC);
}

// Plays the fixture's stream from the start and returns the handle, with the voice already running
// (so the next rendered block begins at sample 0).
static WlAudioHandle play_ready(Fix *f) {
    WlAudioPlay p = music();
    WlAudioHandle h = 0;
    if (wl_audio_stream_play(&f->span, &p, &h) != WL_OK || h == 0) {
        return 0;
    }
    for (int i = 0; i < 4000; i++) {
        wl_audio_pump(NULL, NULL);
        if (wl_audio_state(h) == WL_AUDIO_PLAYING) {
            break;
        }
        wl_platform_thread_sleep_ms(1);
    }
    wl_audio_sync(); // the start command is applied on the next block boundary
    return wl_audio_state(h) == WL_AUDIO_PLAYING ? h : 0;
}

// Renders `frames` from the stream and compares against the reference timeline. `slack_end` frames
// at a non-looping end are not compared (the mixer's look-ahead drops the last couple).
static int compare_stream(const Fix *f, const int16_t *out, uint64_t frames) {
    for (uint64_t i = 0; i < frames; i++) {
        const uint32_t p = pos_at(&f->spec, i);
        if (!f->spec.loop && p >= f->spec.samples - 2) {
            break;
        }
        const int16_t l = f->ref[0][p];
        const int16_t r = f->spec.channels == 2 ? f->ref[1][p] : l;
        if (out[2 * i] != l || out[2 * i + 1] != r) {
            fprintf(stderr, "mismatch at frame %llu (sample %u): got %d/%d want %d/%d\n", (unsigned long long)i, p,
                    out[2 * i], out[2 * i + 1], l, r);
            return 1;
        }
    }
    return 0;
}

static int16_t *g_out;

// ---- tests --------------------------------------------------------------------------------------

TEST pwst_parse_accepts_ours_and_rejects_corruption(void) {
    FxAStreamSpec sp = {32000, 14 * 100, 2, true, 14 * 10, 14 * 100 - 1, 3};
    uint8_t *d;
    size_t n;
    ASSERT_EQ(WL_OK, fxa_build_pwst(&sp, &d, &n, NULL));
    WlStreamInfo in;
    ASSERT_EQ(WL_OK, wl_pwst_parse(d, WL_PWST_MAX_PROBE, n, &in));
    ASSERT_EQ(2u, in.channels);
    ASSERT_EQ(32000u, in.rate);
    ASSERT_EQ(1400u, in.samples);
    ASSERT_EQ(140u, in.loop_start);
    ASSERT_EQ(1399u, in.loop_end);
    ASSERT_EQ(0xA0u, in.data_off);
    ASSERT_EQ(100u * 8u * 2u, in.data_size);
    ASSERT_EQ((int16_t)FX_DSP_COEF[2], in.ch[0].coef[2]);
    ASSERT_EQ((int16_t)FX_DSP_COEF[15], in.ch[1].coef[15]);

    uint8_t bad[0x100];
    struct {
        const char *what;
        size_t off;
        uint8_t val;
        WlResult want;
    } cases[] = {
        {"magic", 0, 'X', WL_ERR_FORMAT},
        {"version", 5, 2, WL_ERR_VERSION},
        {"channels 0", 8, 0, WL_ERR_FORMAT},
        {"channels 3", 8, 3, WL_ERR_FORMAT},
        {"codec 1", 9, 1, WL_ERR_FORMAT},
        {"samples not a multiple of 14", 0x13, 0x41, WL_ERR_FORMAT},
        {"data_off unaligned", 0x1F, 0xA1, WL_ERR_FORMAT},
        {"data_off inside the blocks", 0x1F, 0x20, WL_ERR_FORMAT},
        {"data_size wrong", 0x23, 0x01, WL_ERR_FORMAT},
        {"loop_end past the end", 0x1B, 0xFF, WL_ERR_FORMAT},
        {"loop_start >= loop_end", 0x17, 0xFF, WL_ERR_FORMAT},
        {"unaligned loop without flag 2", 0x17, 0x8D, WL_ERR_FORMAT},
    };
    for (size_t i = 0; i < WL_ARRAY_COUNT(cases); i++) {
        memcpy(bad, d, sizeof bad);
        bad[cases[i].off] = cases[i].val;
        WlResult r = wl_pwst_parse(bad, sizeof bad, n, &in);
        if (r != cases[i].want) {
            fprintf(stderr, "case '%s': got %d want %d\n", cases[i].what, r, cases[i].want);
            free(d);
            FAIL();
        }
    }
    // Payload shorter than data_off + data_size, and probes that are too short.
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwst_parse(d, WL_PWST_MAX_PROBE, n - 32, &in));
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwst_parse(d, 0x40, n, &in));
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwst_parse(d, 0x10, n, &in));
    free(d);
    PASS();
}

TEST synfix_parse_accepts_ours_and_rejects_corruption(void) {
    FxASfxSpec sp = {32728, 14 * 50, 0, WL_BUS_SPEECH, 80, true, 140, 699, -100, 300, 20, 250, 1};
    uint8_t *d;
    size_t n;
    ASSERT_EQ(WL_OK, fxa_build_pwsf(&sp, &d, &n, NULL));
    WlSfxInfo in;
    ASSERT_EQ(WL_OK, wl_pwsf_parse(d, n, &in));
    ASSERT_EQ(32728u, in.rate);
    ASSERT_EQ(700u, in.samples);
    ASSERT_EQ(WL_BUS_SPEECH, in.bus);
    ASSERT_EQ(80, in.priority);
    ASSERT_EQ(-100, in.pitch_cents);
    ASSERT_EQ(300, in.volume);
    ASSERT_EQ(20, in.pan);
    ASSERT_EQ(250, in.play_ms);
    ASSERT_EQ(1u, in.flags & 1u);
    uint8_t *bad = malloc(n);
    memcpy(bad, d, n);
    bad[0] = 'Q';
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwsf_parse(bad, n, &in));
    memcpy(bad, d, n);
    bad[5] = 2;
    ASSERT_EQ(WL_ERR_VERSION, wl_pwsf_parse(bad, n, &in));
    memcpy(bad, d, n);
    bad[0xA] = 9; // bus
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwsf_parse(bad, n, &in));
    memcpy(bad, d, n);
    bad[0x23] = 0x01; // data_size shorter than the samples need
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwsf_parse(bad, n, &in));
    memcpy(bad, d, n);
    bad[0x11] = 0xFF; // samples huge
    bad[0x10] = 0xFF;
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwsf_parse(bad, n, &in));
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwsf_parse(d, n - 64, &in)); // data beyond the payload
    ASSERT_EQ(WL_ERR_FORMAT, wl_pwsf_parse(d, 0x30, &in));
    free(bad);
    free(d);
    PASS();
}

static int run_exact(Fix *f, uint64_t frames) {
    const uint32_t blocks = (uint32_t)((frames + 255u) / 256u);
    g_out = malloc(sizeof(int16_t) * 512u * blocks);
    WlAudioHandle h = play_ready(f);
    if (h == 0) {
        free(g_out);
        return 2;
    }
    run_blocks(g_out, blocks);
    int bad = compare_stream(f, g_out, frames);
    WlAudioStats st;
    wl_audio_stats(&st);
    if (st.starved_frames != 0) {
        fprintf(stderr, "starved %u frames\n", st.starved_frames);
        bad = 3;
    }
    free(g_out);
    wl_audio_stop(h, 0);
    wl_audio_sync();
    wl_host_audio_render(NULL, 512); // the voice fades out over one block and leaves the mixer
    settle();
    return bad;
}

TEST stream_loops_exactly_across_blocks_and_loop_passes(void) {
    SETUP();
    Fix f;
    // 70000 samples: 40000 bytes per channel, so a block ends at the file end and the loop restarts
    // in the next block; three loop passes over 210000 frames.
    FxAStreamSpec sp = {32000, 70000, 2, true, 14000, 69999, 1};
    ASSERT_EQ(WL_OK, fix_make(&f, &sp, "a.bin"));
    ASSERT_EQ(0, run_exact(&f, 210000));
    WlAudioStats st;
    wl_audio_stats(&st);
    ASSERT(st.stream_reads >= 10);
    ASSERT_EQ(0u, st.stream_errors);
    fix_free(&f);
    TEARDOWN();
    PASS();
}

TEST stream_loops_from_the_middle_of_a_frame(void) {
    SETUP();
    Fix f;
    // Loop starts 5 samples into frame 1200 and ends 9 samples into frame 4000 (PWST flag bit 1).
    FxAStreamSpec sp = {32000, 14 * 5000, 2, true, 14 * 1200 + 5, 14 * 4000 + 9, 2};
    ASSERT_EQ(WL_OK, fix_make(&f, &sp, "mid.bin"));
    ASSERT_EQ(0, run_exact(&f, 14ull * 4000 + 9 + 3 * (sp.loop_end - sp.loop_start + 1) + 777));
    fix_free(&f);
    // Mono, loop beginning mid-frame near the start (intro of 3 samples).
    FxAStreamSpec sp2 = {32000, 14 * 3000, 1, true, 3, 14 * 3000 - 1, 4};
    ASSERT_EQ(WL_OK, fix_make(&f, &sp2, "mid2.bin"));
    ASSERT_EQ(0, run_exact(&f, 3ull * 42000 + 100));
    fix_free(&f);
    TEARDOWN();
    PASS();
}

TEST stream_without_loop_plays_to_the_end_and_is_freed(void) {
    SETUP();
    Fix f;
    FxAStreamSpec sp = {32000, 14 * 700, 2, false, 0, 0, 6}; // 9800 samples
    ASSERT_EQ(WL_OK, fix_make(&f, &sp, "end.bin"));
    WlAudioHandle h = play_ready(&f);
    ASSERT(h != 0);
    g_out = malloc(sizeof(int16_t) * 512u * 60u);
    run_blocks(g_out, 60);
    ASSERT_EQ(0, compare_stream(&f, g_out, 9800));
    for (int i = 9800 / 256 + 2; i < 60; i++) { // silence after the end
        ASSERT_EQ(0, g_out[512 * i + 10]);
    }
    settle();
    ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(h));
    WlAudioStats st;
    wl_audio_stats(&st);
    ASSERT_EQ(0u, st.streams_active);
    ASSERT_EQ(0u, st.starved_frames);
    ASSERT_EQ(0u, wl_mem_live_blocks(WL_TAG_AUDIO) - 2u); // mixer and command ring only
    free(g_out);
    fix_free(&f);
    TEARDOWN();
    PASS();
}

TEST stream_at_another_rate_is_resampled(void) {
    SETUP();
    Fix f;
    FxAStreamSpec sp = {44100, 14 * 4000, 1, true, 0, 14 * 4000 - 1, 8};
    ASSERT_EQ(WL_OK, fix_make(&f, &sp, "r44.bin"));
    WlAudioHandle h = play_ready(&f);
    ASSERT(h != 0);
    g_out = malloc(sizeof(int16_t) * 512u * 40u);
    run_blocks(g_out, 40);
    const uint32_t step = wl_mixer_step(44100, 0);
    ASSERT_EQ(90316u, step); // 44100 / 32000 * 65536
    for (uint64_t i = 0; i < 40 * 256 - 4; i++) {
        const uint64_t pos = i * step;
        const uint32_t p = (uint32_t)(pos >> 16);
        const int32_t frac = (int32_t)((pos & 0xFFFF) >> 1);
        const int32_t a = f.ref[0][p], b = f.ref[0][p + 1];
        const int32_t want = a + (((b - a) * frac) >> 15);
        ASSERT(g_out[2 * i] >= want - 1 && g_out[2 * i] <= want + 1);
    }
    free(g_out);
    wl_audio_stop(h, 0);
    wl_audio_sync();
    settle();
    fix_free(&f);
    TEARDOWN();
    PASS();
}

static uint64_t first_silence(const int16_t *out, uint64_t frames, uint64_t from) {
    uint32_t run = 0;
    for (uint64_t i = from; i < frames; i++) {
        run = (out[2 * i] == 0 && out[2 * i + 1] == 0) ? run + 1 : 0;
        if (run == 64) {
            return i - 63;
        }
    }
    return UINT64_MAX;
}

TEST stream_starves_without_the_pump_and_recovers_in_place(void) {
    SETUP();
    Fix f;
    FxAStreamSpec sp = {32000, 14 * 20000, 2, true, 14 * 100, 14 * 20000 - 1, 9};
    ASSERT_EQ(WL_OK, fix_make(&f, &sp, "starve.bin"));
    WlAudioHandle h = play_ready(&f);
    ASSERT(h != 0);
    const uint32_t blocks = 1500; // 12 s of audio without a single pump: the ring holds at most 5.4 s
    int16_t *out = malloc(sizeof(int16_t) * 512u * blocks);
    for (uint32_t b = 0; b < blocks; b++) {
        wl_host_audio_render(out + 512u * b, 256);
    }
    WlAudioStats st;
    wl_audio_stats(&st);
    ASSERT(st.starved_frames > 100000);
    ASSERT_EQ(0u, st.underruns); // the output itself never ran dry: the mixer delivered (silent) blocks
    const uint64_t frames = 256ull * blocks;
    const uint64_t s = first_silence(out, frames, 1000);
    ASSERT(s != UINT64_MAX);
    ASSERT_EQ(0, compare_stream(&f, out, s)); // exact up to the starvation
    // Resume: the pump refills the ring; the stream continues at sample s, no gap in the data.
    int16_t *tail = malloc(sizeof(int16_t) * 512u * 40u);
    run_blocks(tail, 40);
    int found = -1;
    for (int k = 0; k < 64 && found < 0; k++) {
        bool ok = true;
        for (int j = 0; j < 400 && ok; j++) {
            const uint32_t p = pos_at(&sp, s + (uint64_t)j);
            ok = tail[2 * (k + j)] == f.ref[0][p] && tail[2 * (k + j) + 1] == f.ref[1][p];
        }
        if (ok) {
            found = k;
        }
    }
    ASSERT(found >= 0);
    for (int k = 0; k < found; k++) {
        ASSERT_EQ(0, tail[2 * k]);
    }
    free(tail);
    free(out);
    fix_free(&f);
    TEARDOWN();
    PASS();
}

TEST streams_fade_crossfade_and_slot_limit(void) {
    SETUP();
    Fix a, b;
    FxAStreamSpec sa = {32000, 14 * 6000, 2, true, 0, 14 * 6000 - 1, 11};
    FxAStreamSpec sb = {32000, 14 * 6000, 1, true, 14 * 10, 14 * 6000 - 1, 12};
    ASSERT_EQ(WL_OK, fix_make(&a, &sa, "xa.bin"));
    ASSERT_EQ(WL_OK, fix_make(&b, &sb, "xb.bin"));
    WlAudioHandle ha = play_ready(&a);
    ASSERT(ha != 0);
    int16_t out[512];
    run_blocks(NULL, 20);
    // Crossfade: B fades in over 400 ms while A fades out over 400 ms.
    WlAudioPlay p = music();
    p.fade_in_ms = 400;
    WlAudioHandle hb = 0;
    ASSERT_EQ(WL_OK, wl_audio_stream_play(&b.span, &p, &hb));
    ASSERT(wait_state(hb, WL_AUDIO_PLAYING));
    wl_audio_stop(ha, 400);
    WlAudioHandle extra[3] = {0, 0, 0};
    WlAudioPlay q = music();
    ASSERT_EQ(WL_OK, wl_audio_stream_play(&a.span, &q, &extra[0])); // third slot (A is closing, B plays)
    WlAudioHandle none = 0;
    ASSERT_EQ(WL_ERR_LIMIT, wl_audio_stream_play(&a.span, &q, &none)); // all WL_AUDIO_MAX_STREAMS busy
    ASSERT_EQ(0u, none);
    wl_audio_stop(extra[0], 0);
    int ga = 0;
    WlAudioStats sx;
    for (int blk = 0; blk < 120; blk++) { // ~1 s
        settle();
        wl_host_audio_render(out, 256);
        wl_audio_stats(&sx);
        if (sx.streams_active == 1 && ga == 0) { // A's slot is free again: its fade-out finished
            ga = blk;
        }
    }
    ASSERT(ga > 40 && ga < 80); // A faded out in about 400 ms = 50 blocks, plus the block it was cut at
    ASSERT_EQ((int)WL_AUDIO_PLAYING, (int)wl_audio_state(hb));
    ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(extra[0]));
    wl_audio_stop(hb, 100);
    for (int blk = 0; blk < 30; blk++) {
        settle();
        wl_host_audio_render(out, 256);
    }
    ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(hb));
    WlAudioStats st;
    wl_audio_stats(&st);
    ASSERT_EQ(0u, st.streams_active);
    fix_free(&a);
    fix_free(&b);
    TEARDOWN();
    PASS();
}

TEST bad_streams_fail_cleanly_and_do_not_leak_slots(void) {
    SETUP();
    Fix f;
    FxAStreamSpec sp = {32000, 14 * 6000, 2, true, 0, 14 * 6000 - 1, 13};
    ASSERT_EQ(WL_OK, fix_make(&f, &sp, "bad.bin"));
    WlAudioPlay p = music();
    WlAudioHandle h = 0;
    // Immediate argument errors.
    WlAudioSpan s = f.span;
    s.size = 16;
    ASSERT_EQ(WL_ERR_ARG, wl_audio_stream_play(&s, &p, &h));
    s = f.span;
    s.path = NULL;
    ASSERT_EQ(WL_ERR_ARG, wl_audio_stream_play(&s, &p, &h));
    // Not a PWST at all (the filler in front of it).
    s = f.span;
    s.offset = 0;
    s.size = 0x100;
    ASSERT_EQ(WL_OK, wl_audio_stream_play(&s, &p, &h));
    ASSERT(wait_state(h, WL_AUDIO_FAILED));
    // A file that does not exist, and a span that runs off the end of the file.
    s = f.span;
    s.path = "sd:/nothing.bin";
    ASSERT_EQ(WL_OK, wl_audio_stream_play(&s, &p, &h));
    ASSERT(wait_state(h, WL_AUDIO_FAILED));
    s = f.span;
    s.offset = f.size - 100;
    s.size = 4000;
    ASSERT_EQ(WL_OK, wl_audio_stream_play(&s, &p, &h));
    ASSERT(wait_state(h, WL_AUDIO_FAILED));
    // A loop shorter than one block pass is refused (the sample player is for those).
    FxAStreamSpec tiny = {32000, 14 * 6000, 1, true, 14 * 10, 14 * 10 + 13, 14};
    Fix g;
    ASSERT_EQ(WL_OK, fix_make(&g, &tiny, "tiny.bin"));
    ASSERT_EQ(WL_OK, wl_audio_stream_play(&g.span, &p, &h));
    ASSERT(wait_state(h, WL_AUDIO_FAILED));
    for (int i = 0; i < 20; i++) {
        settle();
        wl_platform_thread_sleep_ms(1);
    }
    WlAudioStats st;
    wl_audio_stats(&st);
    ASSERT_EQ(0u, st.streams_active); // every failed stream released its slot
    ASSERT(st.stream_errors >= 4);
    // And a good stream still plays afterwards.
    WlAudioHandle ok = play_ready(&f);
    ASSERT(ok != 0);
    int16_t out[512];
    run_blocks(out, 1);
    ASSERT(out[100] != 0 || out[101] != 0 || out[200] != 0);
    wl_audio_stop(ok, 0);
    wl_audio_sync();
    settle();
    fix_free(&g);
    fix_free(&f);
    TEARDOWN();
    PASS();
}

// Expected output of a mono sample at pan -127 on a unity bus: left = (s * 16383) >> 14, right 0.
static int32_t pan_left(int16_t s) {
    return ((int32_t)s * 16383) >> 14;
}

static WlAudioSfx *load_sfx(const FxASfxSpec *sp, int16_t **ref) {
    uint8_t *d;
    size_t n;
    if (fxa_build_pwsf(sp, &d, &n, ref) != WL_OK) {
        return NULL;
    }
    WlAudioSfx *s = NULL;
    WlResult r = wl_audio_sfx_load(d, n, &s);
    free(d);
    return r == WL_OK ? s : NULL;
}

TEST sfx_plays_decoded_samples_with_loop_pitch_and_length(void) {
    SETUP();
    int16_t *ref = NULL;
    // 1. ADPCM, one shot, hard left.
    FxASfxSpec sp = {32000, 14 * 230, 0, WL_BUS_EFFECTS, 64, false, 0, 0, 0, 256, -127, 0, 5};
    WlAudioSfx *s = load_sfx(&sp, &ref);
    ASSERT(s != NULL);
    ASSERT_EQ(WL_BUS_EFFECTS, wl_audio_sfx_info(s)->bus);
    WlAudioHandle h = wl_audio_sfx_play(s, NULL);
    ASSERT(h != 0);
    int16_t out[512 * 16];
    wl_audio_sync();
    for (int b = 0; b < 16; b++) {
        wl_host_audio_render(out + 512 * b, 256);
    }
    for (uint32_t i = 0; i < 3220; i++) {
        ASSERT_EQ(pan_left(ref[i]), out[2 * i]);
        ASSERT_EQ(0, out[2 * i + 1]);
    }
    for (uint32_t i = 3220; i < 4096; i++) {
        ASSERT_EQ(0, out[2 * i]);
    }
    ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(h));
    free(ref);
    wl_audio_sfx_free(s);

    // 2. Looping: frames 280..1399 repeat.
    FxASfxSpec lp = {32000, 14 * 100, 0, WL_BUS_EFFECTS, 64, true, 280, 1399, 0, 256, -127, 0, 6};
    s = load_sfx(&lp, &ref);
    ASSERT(s != NULL);
    h = wl_audio_sfx_play(s, NULL);
    wl_audio_sync();
    int16_t big[512 * 40];
    for (int b = 0; b < 40; b++) {
        wl_host_audio_render(big + 512 * b, 256);
    }
    for (uint32_t i = 0; i < 40 * 256; i++) {
        const uint32_t p = i <= 1399 ? i : 280 + (i - 1400) % 1120;
        ASSERT_EQ(pan_left(ref[p]), big[2 * i]);
    }
    ASSERT_EQ((int)WL_AUDIO_PLAYING, (int)wl_audio_state(h));
    wl_audio_stop(h, 0);
    wl_audio_sync();
    wl_host_audio_render(NULL, 512);
    ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(h));
    free(ref);
    wl_audio_sfx_free(s);

    // 3. One octave up (reads every other sample), PCM16 codec, explicit parameters.
    FxASfxSpec pc = {32000, 6000, 1, WL_BUS_SPEECH, 64, false, 0, 0, 0, 256, 0, 0, 7};
    s = load_sfx(&pc, &ref);
    ASSERT(s != NULL);
    WlAudioPlay pl = wl_audio_sfx_params(s);
    pl.pan = -127;
    pl.pitch_cents = 1200;
    pl.bus = WL_BUS_EFFECTS;
    h = wl_audio_sfx_play(s, &pl);
    wl_audio_sync();
    for (int b = 0; b < 12; b++) {
        wl_host_audio_render(big + 512 * b, 256);
    }
    for (uint32_t i = 0; i < 2900; i++) {
        if (pan_left(ref[2 * i]) != big[2 * i]) {
            fprintf(stderr, "frame %u: got %d want %d (ref %d)\n", i, big[2 * i], pan_left(ref[2 * i]), ref[2 * i]);
            FAIL();
        }
    }
    free(ref);
    wl_audio_sfx_free(s);

    // 4. play_ms limits a one-shot: 100 ms at 32 kHz is 3200 of its 14000 samples.
    FxASfxSpec pm = {32000, 14 * 1000, 0, WL_BUS_EFFECTS, 64, false, 0, 0, 0, 256, 0, 100, 8};
    s = load_sfx(&pm, NULL);
    ASSERT(s != NULL);
    h = wl_audio_sfx_play(s, NULL);
    wl_audio_sync();
    wl_host_audio_render(NULL, 256 * 10);
    ASSERT_EQ((int)WL_AUDIO_PLAYING, (int)wl_audio_state(h));
    wl_host_audio_render(NULL, 256 * 4);
    ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(h));
    wl_audio_sfx_free(s);
    TEARDOWN();
    PASS();
}

TEST sfx_free_stops_its_voices_and_nothing_leaks(void) {
    SETUP();
    FxASfxSpec lp = {32000, 14 * 100, 0, WL_BUS_EFFECTS, 64, true, 0, 1399, 0, 256, 0, 0, 3};
    WlAudioSfx *s = load_sfx(&lp, NULL);
    ASSERT(s != NULL);
    WlAudioHandle h[8];
    for (int i = 0; i < 8; i++) {
        h[i] = wl_audio_sfx_play(s, NULL);
        ASSERT(h[i] != 0);
    }
    wl_audio_sync();
    wl_host_audio_render(NULL, 256 * 3);
    WlAudioStats st;
    wl_audio_stats(&st);
    ASSERT_EQ(8u, st.mix.active);
    wl_audio_sfx_free(s); // stops the voices before the memory goes (ASAN checks the rest)
    wl_host_audio_render(NULL, 256 * 3);
    wl_audio_stats(&st);
    ASSERT_EQ(0u, st.mix.active);
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(h[i]));
    }
    // More than 32 voices: the extra ones steal or drop, nothing breaks.
    s = load_sfx(&lp, NULL);
    for (int i = 0; i < 45; i++) {
        WlAudioPlay p = wl_audio_sfx_params(s);
        p.priority = (uint8_t)(i % 2 ? 30 : 90);
        (void)wl_audio_sfx_play(s, &p);
    }
    wl_audio_sync();
    wl_host_audio_render(NULL, 256);
    wl_audio_stats(&st);
    ASSERT_EQ(32u, st.mix.active);
    ASSERT(st.mix.steals + st.mix.drops >= 13);
    wl_audio_sfx_free(s);
    TEARDOWN();
    PASS();
}

TEST engine_controls_buses_ducking_and_command_ring(void) {
    SETUP();
    // A constant-ish loop on the music bus; a speech loop on the speech bus.
    FxASfxSpec mu = {32000, 1400, 1, WL_BUS_MUSIC, 64, true, 0, 1399, 0, 256, -127, 0, 15};
    FxASfxSpec cr = {32000, 1400, 1, WL_BUS_SPEECH, 64, true, 0, 1399, 0, 10, -127, 0, 16};
    int16_t *rm = NULL;
    WlAudioSfx *sm = load_sfx(&mu, &rm);
    WlAudioSfx *sc = load_sfx(&cr, NULL);
    ASSERT(sm != NULL && sc != NULL);
    WlAudioHandle hm = wl_audio_sfx_play(sm, NULL);
    wl_audio_sync();
    int16_t out[512];
    int32_t peak0 = 0;
    for (int b = 0; b < 12; b++) {
        wl_host_audio_render(out, 256);
    }
    for (int i = 0; i < 256; i++) {
        peak0 = out[2 * i] > peak0 ? out[2 * i] : peak0;
    }
    ASSERT(peak0 > 3000);
    // Speech plays: the music bus ducks to half within a few blocks (the speech itself is quiet).
    WlAudioHandle hc = wl_audio_sfx_play(sc, NULL);
    ASSERT(hc != 0);
    wl_audio_sync();
    for (int b = 0; b < 12; b++) {
        wl_host_audio_render(out, 256);
    }
    int32_t peak1 = 0;
    for (int i = 0; i < 256; i++) {
        peak1 = out[2 * i] > peak1 ? out[2 * i] : peak1;
    }
    ASSERT(peak1 < peak0 * 6 / 10 && peak1 > peak0 * 4 / 10); // about 0.5 (+ the quiet speech)
    wl_audio_stop(hc, 0);
    // Bus volume to zero, master to half.
    wl_audio_bus_volume(WL_BUS_MUSIC, 0);
    wl_audio_sync();
    for (int b = 0; b < 3; b++) {
        wl_host_audio_render(out, 256);
    }
    ASSERT_EQ(0, out[2 * 100]);
    wl_audio_bus_volume(WL_BUS_MUSIC, 256);
    wl_audio_speech_duck(256); // no speech ducking now
    wl_audio_master_volume(128);
    wl_audio_duck_music(256);
    wl_audio_sync();
    for (int b = 0; b < 60; b++) {
        wl_host_audio_render(out, 256);
    }
    int32_t peak2 = 0;
    for (int i = 0; i < 256; i++) {
        peak2 = out[2 * i] > peak2 ? out[2 * i] : peak2;
    }
    ASSERT(peak2 > peak0 * 4 / 10 && peak2 < peak0 * 6 / 10);
    wl_audio_master_volume(256);
    wl_audio_stop_all();
    wl_audio_sync();
    wl_host_audio_render(out, 256);
    wl_host_audio_render(out, 256);
    ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(hm));
    free(rm);

    // The command ring holds 256: flooding it without rendering drops the surplus, counts it,
    // and everything works again once blocks drain it (96 commands per block).
    for (int i = 0; i < 300; i++) {
        wl_audio_set(hm, 256, 0, 0);
    }
    WlAudioStats st;
    wl_audio_stats(&st);
    ASSERT(st.cmd_drops >= 40);
    ASSERT_EQ(0u, wl_audio_sfx_play(sm, NULL)); // full: no handle
    for (int b = 0; b < 4; b++) {
        wl_host_audio_render(NULL, 256);
    }
    ASSERT(wl_audio_sfx_play(sm, NULL) != 0);
    wl_audio_sfx_free(sm);
    wl_audio_sfx_free(sc);
    TEARDOWN();
    PASS();
}

TEST streaming_with_a_real_audio_thread(void) {
    SETUP();
    Fix f;
    FxAStreamSpec sp = {32000, 14 * 8000, 2, true, 14 * 500 + 3, 14 * 8000 - 1, 17};
    ASSERT_EQ(WL_OK, fix_make(&f, &sp, "thr.bin"));
    ASSERT_EQ(WL_OK, wl_host_audio_thread_start());
    ASSERT(wl_platform_audio_threaded());
    WlAudioPlay p = music();
    WlAudioHandle h = 0;
    ASSERT_EQ(WL_OK, wl_audio_stream_play(&f.span, &p, &h));
    FxASfxSpec fx = {32000, 14 * 100, 0, WL_BUS_EFFECTS, 64, false, 0, 0, 0, 256, 0, 0, 21};
    WlAudioSfx *s = load_sfx(&fx, NULL);
    ASSERT(s != NULL);
    // About 2.5 s of wall time: the thread mixes 8x faster than real time, so ~20 s of audio.
    uint64_t t0 = wl_platform_fs_clock_us();
    uint32_t shots = 0;
    while (wl_platform_fs_clock_us() - t0 < 2500000u) {
        wl_audio_pump(NULL, NULL);
        if (shots < 400 && wl_audio_sfx_play(s, NULL) != 0) {
            shots++;
        }
        wl_platform_thread_sleep_ms(3);
    }
    WlAudioStats st;
    wl_audio_stats(&st);
    ASSERT_EQ((int)WL_AUDIO_PLAYING, (int)wl_audio_state(h));
    ASSERT(st.blocks_filled > 1500);
    ASSERT_EQ(0u, st.starved_frames);
    ASSERT_EQ(0u, st.stream_errors);
    ASSERT(shots > 100);
    wl_audio_stop(h, 50);
    wl_audio_sfx_free(s);
    ASSERT(wait_state(h, WL_AUDIO_GONE));
    for (int i = 0; i < 100; i++) {
        wl_audio_pump(NULL, NULL);
        wl_audio_stats(&st);
        if (st.streams_active == 0) {
            break;
        }
        wl_platform_thread_sleep_ms(2);
    }
    ASSERT_EQ(0u, st.streams_active);
    wl_host_audio_thread_stop();
    fix_free(&f);
    TEARDOWN();
    PASS();
}

static void dummy_fill(void *user, int16_t *out, uint32_t frames) {
    (void)user;
    memset(out, 0, (size_t)frames * 4u);
}

TEST lifecycle_and_calls_on_a_stopped_engine(void) {
    engine_up();
    ASSERT_EQ(WL_OK, wl_audio_init());
    ASSERT(wl_audio_ready());
    ASSERT_EQ(WL_ERR_STATE, wl_audio_init());
    ASSERT_EQ(WL_ERR_STATE, wl_platform_audio_start(32000, 256, dummy_fill, NULL)); // output already started
    wl_audio_shutdown();
    ASSERT(!wl_audio_ready());
    ASSERT_EQ(0u, wl_mem_live_blocks(WL_TAG_AUDIO));
    // Everything is a no-op or an error without an engine.
    ASSERT_EQ(0u, wl_audio_sfx_play(NULL, NULL));
    wl_audio_stop(5, 0);
    wl_audio_stop_all();
    wl_audio_sync();
    wl_audio_pump(NULL, NULL);
    ASSERT_EQ((int)WL_AUDIO_GONE, (int)wl_audio_state(5));
    WlAudioHandle h;
    WlAudioPlay p = music();
    WlAudioSpan span = {"sd:/x", 0, 1000};
    ASSERT_EQ(WL_ERR_STATE, wl_audio_stream_play(&span, &p, &h));
    // Argument checks of the platform output.
    ASSERT_EQ(WL_ERR_ARG, wl_platform_audio_start(32000, 256, NULL, NULL));
    // It can be restarted, and a second life starts from clean state.
    ASSERT_EQ(WL_OK, engine_start());
    engine_stop();
    ASSERT_EQ(WL_OK, engine_start());
    engine_stop();
    ASSERT_EQ(0u, wl_mem_live_blocks(WL_TAG_AUDIO));
    PASS();
}

SUITE(engine_suite) {
    RUN_TEST(pwst_parse_accepts_ours_and_rejects_corruption);
    RUN_TEST(synfix_parse_accepts_ours_and_rejects_corruption);
    RUN_TEST(stream_loops_exactly_across_blocks_and_loop_passes);
    RUN_TEST(stream_loops_from_the_middle_of_a_frame);
    RUN_TEST(stream_without_loop_plays_to_the_end_and_is_freed);
    RUN_TEST(stream_at_another_rate_is_resampled);
    RUN_TEST(stream_starves_without_the_pump_and_recovers_in_place);
    RUN_TEST(streams_fade_crossfade_and_slot_limit);
    RUN_TEST(bad_streams_fail_cleanly_and_do_not_leak_slots);
    RUN_TEST(sfx_plays_decoded_samples_with_loop_pitch_and_length);
    RUN_TEST(sfx_free_stops_its_voices_and_nothing_leaks);
    RUN_TEST(engine_controls_buses_ducking_and_command_ring);
    RUN_TEST(streaming_with_a_real_audio_thread);
    RUN_TEST(lifecycle_and_calls_on_a_stopped_engine);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    const char *base = getenv("TMPDIR");
    snprintf(g_sd, sizeof g_sd, "%s/pwaudio_sd_XXXXXX", base != NULL ? base : "/tmp");
    if (mkdtemp(g_sd) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(engine_suite);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_sd);
    int rc = system(cmd);
    (void)rc;
    GREATEST_MAIN_END();
}
