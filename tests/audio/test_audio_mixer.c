// SPDX-License-Identifier: MIT
// The software mixer (wl/audio/mixer.c): identity at unity, pan law, buses, clipping, loop points,
// pitch, fades, voice stealing, ducking, pull sources, determinism.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wl/audio/mixer.h"
#include "wl/base/hash.h"
#include "greatest.h"

static WlMixer g_m;

static void reset(void) {
    wl_mixer_init(&g_m);
    wl_mixer_bus_set(&g_m, WL_BUS_MUSIC, 256); // unity everywhere unless a test says otherwise
    wl_mixer_bus_set(&g_m, WL_BUS_UI, 256);
}

static WlMixVoiceDesc sample_desc(uint32_t id, const int16_t *pcm, uint32_t frames, bool stereo, WlBus bus) {
    WlMixVoiceDesc d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.bus = (uint8_t)bus;
    d.priority = 64;
    d.gain = 256;
    d.rate = 32000;
    d.pcm = pcm;
    d.frames = frames;
    d.stereo = stereo;
    return d;
}

TEST silence_without_voices(void) {
    reset();
    int16_t out[512];
    memset(out, 0x77, sizeof out);
    wl_mixer_mix(&g_m, out, 256);
    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(0, out[i]);
    }
    ASSERT_EQ(1u, g_m.stats.blocks);
    ASSERT_EQ(0u, g_m.stats.active);
    PASS();
}

TEST stereo_source_at_unity_is_the_identity(void) {
    reset();
    static int16_t src[2 * 1000];
    for (int i = 0; i < 1000; i++) {
        src[2 * i] = (int16_t)(i * 31 - 15000);
        src[2 * i + 1] = (int16_t)(12000 - i * 23);
    }
    WlMixVoiceDesc d = sample_desc(1, src, 1000, true, WL_BUS_EFFECTS);
    ASSERT_EQ(0, wl_mixer_voice_start(&g_m, &d));
    static int16_t out[2 * 1280];
    for (int b = 0; b < 5; b++) {
        wl_mixer_mix(&g_m, out + 2 * 256 * b, 256);
    }
    ASSERT_MEM_EQ(src, out, sizeof src);
    for (int i = 2000; i < 2560; i++) {
        ASSERT_EQ(0, out[i]); // ended at frame 1000
    }
    ASSERT_EQ(0u, g_m.stats.active);
    ASSERT(!wl_mixer_voice_active(&g_m, 1));
    PASS();
}

static int16_t mono_const[2000];

TEST mono_pan_law_is_constant_power(void) {
    for (int i = 0; i < 2000; i++) {
        mono_const[i] = 10000;
    }
    int64_t prev_power = 0;
    for (int pan = -127; pan <= 127; pan += 1) {
        reset();
        WlMixVoiceDesc d = sample_desc(1, mono_const, 2000, false, WL_BUS_EFFECTS);
        d.pan = (int8_t)pan;
        wl_mixer_voice_start(&g_m, &d);
        int16_t out[512];
        wl_mixer_mix(&g_m, out, 256);
        const int64_t l = out[2 * 100], r = out[2 * 100 + 1];
        const int64_t power = l * l + r * r;
        // Constant to within 1 % of 10000^2.
        ASSERT(power > 99000000LL && power < 101000000LL);
        if (pan == 0) {
            ASSERT(l > 7000 && l < 7140 && r > 7000 && r < 7140); // -3 dB each
            ASSERT(l - r <= 2 && r - l <= 2);
        }
        if (pan == -127) {
            ASSERT(l >= 9998 && r == 0);
        }
        if (pan == 127) {
            ASSERT(r >= 9998 && l == 0);
        }
        (void)prev_power;
    }
    PASS();
}

TEST stereo_balance_leaves_the_centre_alone(void) {
    reset();
    static int16_t src[2 * 600];
    for (int i = 0; i < 600; i++) {
        src[2 * i] = 8000;
        src[2 * i + 1] = 8000;
    }
    WlMixVoiceDesc d = sample_desc(1, src, 600, true, WL_BUS_EFFECTS);
    d.pan = 127;
    wl_mixer_voice_start(&g_m, &d);
    int16_t out[1024];
    wl_mixer_mix(&g_m, out, 256);
    wl_mixer_mix(&g_m, out, 256);
    ASSERT_EQ(0, out[2 * 100]);        // left gone at full right
    ASSERT_EQ(8000, out[2 * 100 + 1]); // right unchanged
    PASS();
}

TEST default_bus_gains_and_volume_controls(void) {
    wl_mixer_init(&g_m); // defaults: music 80 %, effects 100 %, speech 100 %, ui 70 %
    for (int i = 0; i < 2000; i++) {
        mono_const[i] = 10000;
    }
    int16_t out[512];
    static const int expect_pct[4] = {80, 100, 100, 70};
    for (int bus = 0; bus < 4; bus++) {
        wl_mixer_init(&g_m);
        WlMixVoiceDesc d = sample_desc(1, mono_const, 2000, false, (WlBus)bus);
        wl_mixer_voice_start(&g_m, &d);
        for (int b = 0; b < 3; b++) {
            wl_mixer_mix(&g_m, out, 256);
        }
        // 10000 * 0.7071 * pct
        const int want = 7071 * expect_pct[bus] / 100;
        ASSERT(out[2 * 100] > want - 40 && out[2 * 100] < want + 40);
    }
    // Bus volume 0 and master.
    wl_mixer_init(&g_m);
    WlMixVoiceDesc d = sample_desc(1, mono_const, 2000, false, WL_BUS_EFFECTS);
    wl_mixer_voice_start(&g_m, &d);
    wl_mixer_bus_set(&g_m, WL_BUS_EFFECTS, 0);
    wl_mixer_mix(&g_m, out, 256); // ramps down across this block
    wl_mixer_mix(&g_m, out, 256);
    ASSERT_EQ(0, out[2 * 100]);
    wl_mixer_bus_set(&g_m, WL_BUS_EFFECTS, 256);
    wl_mixer_master_set(&g_m, 128);
    wl_mixer_mix(&g_m, out, 256);
    wl_mixer_mix(&g_m, out, 256);
    ASSERT(out[2 * 100] > 3500 && out[2 * 100] < 3570); // 7071 / 2
    PASS();
}

TEST output_saturates_and_counts_clipping(void) {
    reset();
    static int16_t big[2 * 600];
    static int16_t neg[2 * 600];
    for (int i = 0; i < 600; i++) {
        big[2 * i] = big[2 * i + 1] = 32767;
        neg[2 * i] = neg[2 * i + 1] = -32768;
    }
    for (uint32_t i = 0; i < 4; i++) {
        WlMixVoiceDesc d = sample_desc(1 + i, big, 600, true, WL_BUS_EFFECTS);
        d.gain = 511;
        wl_mixer_voice_start(&g_m, &d);
    }
    int16_t out[512];
    wl_mixer_mix(&g_m, out, 256);
    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(32767, out[i]);
    }
    ASSERT_EQ(512u, g_m.stats.clipped);
    ASSERT_EQ(32767u, g_m.stats.peak);
    reset();
    for (uint32_t i = 0; i < 4; i++) {
        WlMixVoiceDesc d = sample_desc(1 + i, neg, 600, true, WL_BUS_EFFECTS);
        d.gain = 511;
        wl_mixer_voice_start(&g_m, &d);
    }
    wl_mixer_mix(&g_m, out, 256);
    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(-32768, out[i]); // no wrap-around to positive
    }
    // 60 voices' worth into one bus accumulator cannot overflow it either.
    reset();
    for (uint32_t i = 0; i < 32; i++) {
        WlMixVoiceDesc d = sample_desc(1 + i, big, 600, true, WL_BUS_EFFECTS);
        d.gain = 511;
        wl_mixer_voice_start(&g_m, &d);
    }
    wl_mixer_mix(&g_m, out, 256);
    ASSERT_EQ(32767, out[0]);
    PASS();
}

TEST loop_points_wrap_exactly(void) {
    reset();
    static int16_t src[2 * 400];
    for (int i = 0; i < 400; i++) {
        src[2 * i] = (int16_t)i;
        src[2 * i + 1] = (int16_t)(-i);
    }
    WlMixVoiceDesc d = sample_desc(1, src, 400, true, WL_BUS_EFFECTS);
    d.loop = true;
    d.loop_start = 100;
    d.loop_end = 300; // frames 100..299 repeat; 300..399 are never played
    ASSERT_EQ(0, wl_mixer_voice_start(&g_m, &d));
    static int16_t out[2 * 1024];
    for (int b = 0; b < 4; b++) {
        wl_mixer_mix(&g_m, out + 2 * 256 * b, 256);
    }
    for (int i = 0; i < 1024; i++) {
        const int want = i < 300 ? i : 100 + (i - 300) % 200;
        ASSERT_EQ(want, out[2 * i]);
        ASSERT_EQ(-want, out[2 * i + 1]);
    }
    ASSERT(wl_mixer_voice_active(&g_m, 1));
    // Invalid loop descriptions are refused.
    d.id = 2;
    d.loop_end = 401;
    ASSERT_EQ(-1, wl_mixer_voice_start(&g_m, &d));
    d.loop_end = 100;
    ASSERT_EQ(-1, wl_mixer_voice_start(&g_m, &d));
    PASS();
}

TEST loop_wraps_at_a_pitched_step_without_drifting(void) {
    reset();
    static int16_t src[2 * 64];
    for (int i = 0; i < 64; i++) {
        src[2 * i] = src[2 * i + 1] = (int16_t)(i * 100);
    }
    WlMixVoiceDesc d = sample_desc(1, src, 64, true, WL_BUS_EFFECTS);
    d.loop = true;
    d.loop_start = 0;
    d.loop_end = 64;
    d.pitch_cents = 1200; // exactly 2x
    wl_mixer_voice_start(&g_m, &d);
    static int16_t out[2 * 256];
    wl_mixer_mix(&g_m, out, 256);
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ((2 * i % 64) * 100, out[2 * i]);
    }
    PASS();
}

TEST pitch_steps_and_interpolation(void) {
    ASSERT_EQ(65536u, wl_mixer_step(32000, 0));
    ASSERT_EQ(98304u, wl_mixer_step(48000, 0));
    ASSERT_EQ(131072u, wl_mixer_step(32000, 1200));
    ASSERT_EQ(32768u, wl_mixer_step(32000, -1200));
    ASSERT_EQ(262144u, wl_mixer_step(32000, 2400)); // two octaves: the cap
    ASSERT_EQ(262144u, wl_mixer_step(32000, 9999));
    ASSERT_EQ(1024u, wl_mixer_step(8000, -9999));
    const uint32_t s100 = wl_mixer_step(32000, 100); // 2^(1/12) = 1.059463 -> 69433.4
    ASSERT(s100 >= 69430u && s100 <= 69437u);
    const uint32_t s700 = wl_mixer_step(32000, 700); // 1.498307 -> 98197
    ASSERT(s700 >= 98190u && s700 <= 98205u);
    const uint32_t sm = wl_mixer_step(32000, -50); // 0.971532 -> 63670
    ASSERT(sm >= 63665u && sm <= 63675u);

    reset();
    static int16_t ramp[2 * 1000];
    for (int i = 0; i < 1000; i++) {
        ramp[2 * i] = ramp[2 * i + 1] = (int16_t)(i * 20);
    }
    WlMixVoiceDesc d = sample_desc(1, ramp, 1000, true, WL_BUS_EFFECTS);
    d.pitch_cents = -1200; // half speed: linear interpolation of a ramp is exact
    wl_mixer_voice_start(&g_m, &d);
    static int16_t out[2 * 512];
    wl_mixer_mix(&g_m, out, 256);
    wl_mixer_mix(&g_m, out + 512, 256);
    for (int i = 0; i < 400; i++) {
        const int want = i * 10; // frame i/2 of a 20-per-frame ramp
        ASSERT(out[2 * i] >= want - 1 && out[2 * i] <= want + 1);
    }
    PASS();
}

TEST fades_in_and_out(void) {
    reset();
    for (int i = 0; i < 2000; i++) {
        mono_const[i] = 20000;
    }
    static int16_t src[2 * 20000];
    for (int i = 0; i < 20000; i++) {
        src[2 * i] = src[2 * i + 1] = 20000;
    }
    WlMixVoiceDesc d = sample_desc(1, src, 20000, true, WL_BUS_EFFECTS);
    d.fade_in_frames = 1024;
    wl_mixer_voice_start(&g_m, &d);
    int16_t out[512];
    int prev = -1;
    for (int b = 0; b < 4; b++) {
        wl_mixer_mix(&g_m, out, 256);
        int last = out[2 * 255];
        ASSERT(last > prev);
        prev = last;
    }
    ASSERT(prev >= 19900); // full level after 1024 frames
    wl_mixer_voice_stop(&g_m, 1, 1024);
    prev = 40000;
    for (int b = 0; b < 4; b++) {
        wl_mixer_mix(&g_m, out, 256);
        int last = out[2 * 255];
        ASSERT(last < prev);
        prev = last;
    }
    ASSERT(prev <= 300);
    ASSERT(!wl_mixer_voice_active(&g_m, 1));
    wl_mixer_mix(&g_m, out, 256);
    ASSERT_EQ(0, out[10]);
    // stop(0) still fades over one block instead of clicking.
    reset();
    d = sample_desc(1, src, 20000, true, WL_BUS_EFFECTS);
    wl_mixer_voice_start(&g_m, &d);
    wl_mixer_mix(&g_m, out, 256);
    wl_mixer_voice_stop(&g_m, 1, 0);
    wl_mixer_mix(&g_m, out, 256);
    for (int i = 1; i < 256; i++) {
        ASSERT(out[2 * i] <= out[2 * (i - 1)]); // monotone ramp down
    }
    ASSERT(out[2 * 255] < 200);
    ASSERT(!wl_mixer_voice_active(&g_m, 1));
    PASS();
}

TEST gain_changes_ramp_without_steps(void) {
    reset();
    static int16_t src[2 * 4000];
    for (int i = 0; i < 4000; i++) {
        src[2 * i] = src[2 * i + 1] = 16000;
    }
    WlMixVoiceDesc d = sample_desc(1, src, 4000, true, WL_BUS_EFFECTS);
    wl_mixer_voice_start(&g_m, &d);
    static int16_t out[2 * 512];
    wl_mixer_mix(&g_m, out, 256);
    wl_mixer_voice_set(&g_m, 1, 0, 0, 0);
    wl_mixer_mix(&g_m, out + 512, 256);
    int max_step = 0;
    for (int i = 1; i < 512; i++) {
        int d2 = out[2 * i] - out[2 * (i - 1)];
        d2 = d2 < 0 ? -d2 : d2;
        max_step = d2 > max_step ? d2 : max_step;
    }
    ASSERT(max_step <= 70); // 16000 / 256 = 62.5 per frame, never a jump
    ASSERT_EQ(0, out[2 * 511]);
    PASS();
}

TEST voice_stealing_follows_priority_then_age(void) {
    reset();
    static int16_t src[2 * 2000];
    memset(src, 0, sizeof src);
    for (uint32_t i = 0; i < 32; i++) {
        WlMixVoiceDesc d = sample_desc(100 + i, src, 2000, true, WL_BUS_EFFECTS);
        d.priority = (uint8_t)(i == 5 ? 10 : 64);
        ASSERT_EQ((int)i, wl_mixer_voice_start(&g_m, &d));
    }
    WlMixVoiceDesc d = sample_desc(500, src, 2000, true, WL_BUS_EFFECTS);
    d.priority = 5; // below everything running
    ASSERT_EQ(-1, wl_mixer_voice_start(&g_m, &d));
    ASSERT_EQ(1u, g_m.stats.drops);
    d.priority = 10; // equal to the lowest (voice 105): it is replaced
    ASSERT_EQ(5, wl_mixer_voice_start(&g_m, &d));
    ASSERT(!wl_mixer_voice_active(&g_m, 105));
    ASSERT(wl_mixer_voice_active(&g_m, 500));
    d.id = 501;
    d.priority = 64; // the lowest priority goes first: the priority-10 voice just started (id 500)
    ASSERT_EQ(5, wl_mixer_voice_start(&g_m, &d));
    ASSERT(!wl_mixer_voice_active(&g_m, 500));
    d.id = 502; // now every voice is priority 64: the oldest (id 100, slot 0) goes
    ASSERT_EQ(0, wl_mixer_voice_start(&g_m, &d));
    ASSERT(!wl_mixer_voice_active(&g_m, 100));
    ASSERT_EQ(3u, g_m.stats.steals);
    d.id = 0;
    ASSERT_EQ(-1, wl_mixer_voice_start(&g_m, &d)); // id 0 is invalid
    PASS();
}

TEST speech_ducks_the_music_bus(void) {
    reset();
    static int16_t music[2 * 40000];
    static int16_t speech[2 * 40000];
    for (int i = 0; i < 40000; i++) {
        music[2 * i] = music[2 * i + 1] = 10000;
        speech[2 * i] = speech[2 * i + 1] = 100; // quiet: only its presence matters
    }
    WlMixVoiceDesc m = sample_desc(1, music, 40000, true, WL_BUS_MUSIC);
    wl_mixer_voice_start(&g_m, &m);
    int16_t out[512];
    for (int b = 0; b < 4; b++) {
        wl_mixer_mix(&g_m, out, 256);
    }
    ASSERT_EQ(10000, out[2 * 100]);
    WlMixVoiceDesc c = sample_desc(2, speech, 40000, true, WL_BUS_SPEECH);
    wl_mixer_voice_start(&g_m, &c);
    for (int b = 0; b < 6; b++) {
        wl_mixer_mix(&g_m, out, 256);
    }
    ASSERT(out[2 * 100] >= 5050 && out[2 * 100] <= 5150); // 10000 * 0.5 + the speech voice's 100
    wl_mixer_voice_stop(&g_m, 2, 0);
    for (int b = 0; b < 4; b++) {
        wl_mixer_mix(&g_m, out, 256);
    }
    ASSERT(out[2 * 100] < 7000); // recovering slowly
    for (int b = 0; b < 40; b++) {
        wl_mixer_mix(&g_m, out, 256);
    }
    ASSERT_EQ(10000, out[2 * 100]);
    // Explicit duck (a jingle).
    wl_mixer_duck_set(&g_m, 64);
    for (int b = 0; b < 8; b++) {
        wl_mixer_mix(&g_m, out, 256);
    }
    ASSERT_EQ(2500, out[2 * 100]);
    PASS();
}

typedef struct {
    uint32_t next;   // next source value
    uint32_t pulled; // total frames requested
    uint32_t end_at; // value count after which `ended` is raised (0 = never)
} Counter;

static void counter_pull(void *user, int16_t *st, uint32_t frames, bool *ended) {
    Counter *c = user;
    for (uint32_t i = 0; i < frames; i++) {
        st[2 * i] = (int16_t)(c->next * 2);
        st[2 * i + 1] = (int16_t)(-(int32_t)c->next * 2);
        c->next++;
    }
    c->pulled += frames;
    if (c->end_at != 0 && c->next >= c->end_at) {
        *ended = true;
    }
}

TEST pull_source_unity_pitched_and_ended(void) {
    reset();
    Counter c = {0, 0, 0};
    WlMixVoiceDesc d;
    memset(&d, 0, sizeof d);
    d.id = 1;
    d.bus = WL_BUS_EFFECTS;
    d.priority = 64;
    d.gain = 256;
    d.rate = 32000;
    d.pull = counter_pull;
    d.pull_user = &c;
    wl_mixer_voice_start(&g_m, &d);
    static int16_t out[2 * 1024];
    for (int b = 0; b < 4; b++) {
        wl_mixer_mix(&g_m, out + 512 * b, 256);
    }
    for (int i = 0; i < 1024; i++) {
        ASSERT_EQ(i * 2, out[2 * i]);
        ASSERT_EQ(-i * 2, out[2 * i + 1]);
    }
    ASSERT(c.pulled <= 1024 + 2); // at most two frames of look-ahead

    // 1.5x speed: output i is source frame 1.5 i (interpolated).
    reset();
    c = (Counter){0, 0, 0};
    d.pitch_cents = 702; // ~ 3/2
    wl_mixer_voice_start(&g_m, &d);
    wl_mixer_mix(&g_m, out, 256);
    wl_mixer_mix(&g_m, out + 512, 256);
    const double ratio = wl_mixer_step(32000, 702) / 65536.0;
    for (int i = 0; i < 400; i++) {
        const double want = 2.0 * ratio * i;
        ASSERT(out[2 * i] > want - 3.0 && out[2 * i] < want + 3.0);
    }
    // Ends: the voice finishes by itself.
    reset();
    c = (Counter){0, 0, 600};
    d.pitch_cents = 0;
    wl_mixer_voice_start(&g_m, &d);
    for (int b = 0; b < 4; b++) {
        wl_mixer_mix(&g_m, out, 256);
    }
    ASSERT(!wl_mixer_voice_active(&g_m, 1));
    PASS();
}

TEST stop_pcm_removes_voices_using_a_sample(void) {
    reset();
    static int16_t a[2 * 600], b[2 * 600];
    WlMixVoiceDesc da = sample_desc(1, a, 600, true, WL_BUS_EFFECTS);
    WlMixVoiceDesc db = sample_desc(2, b, 600, true, WL_BUS_EFFECTS);
    wl_mixer_voice_start(&g_m, &da);
    da.id = 3;
    wl_mixer_voice_start(&g_m, &da);
    wl_mixer_voice_start(&g_m, &db);
    wl_mixer_stop_pcm(&g_m, a);
    ASSERT(!wl_mixer_voice_active(&g_m, 1));
    ASSERT(!wl_mixer_voice_active(&g_m, 3));
    ASSERT(wl_mixer_voice_active(&g_m, 2));
    wl_mixer_stop_all(&g_m);
    ASSERT(!wl_mixer_voice_active(&g_m, 2));
    PASS();
}

TEST partial_blocks_are_allowed(void) {
    reset();
    static int16_t src[2 * 200];
    for (int i = 0; i < 200; i++) {
        src[2 * i] = (int16_t)i;
        src[2 * i + 1] = (int16_t)i;
    }
    WlMixVoiceDesc d = sample_desc(1, src, 200, true, WL_BUS_EFFECTS);
    wl_mixer_voice_start(&g_m, &d);
    int16_t out[2 * 64];
    wl_mixer_mix(&g_m, out, 64);
    wl_mixer_mix(&g_m, out, 0);   // ignored
    wl_mixer_mix(&g_m, out, 999); // too big: ignored
    ASSERT_EQ(1u, g_m.stats.blocks);
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(i, out[2 * i]);
    }
    wl_mixer_mix(&g_m, out, 64);
    ASSERT_EQ(64, out[0]);
    PASS();
}

// A fixed scenario hashed: the output must not change unintentionally.
TEST regression_hash_of_a_mixed_scene(void) {
    reset();
    wl_mixer_init(&g_m);
    static int16_t src[2 * 3000];
    static int16_t mono[3000];
    uint32_t x = 99;
    for (int i = 0; i < 3000; i++) {
        x = x * 1664525u + 1013904223u;
        src[2 * i] = (int16_t)((x >> 16) / 3);
        x = x * 1664525u + 1013904223u;
        src[2 * i + 1] = (int16_t)((x >> 16) / 3);
        mono[i] = (int16_t)(src[2 * i] / 2);
    }
    WlMixVoiceDesc a = sample_desc(1, src, 3000, true, WL_BUS_MUSIC);
    a.loop = true;
    a.loop_start = 500;
    a.loop_end = 2900;
    a.pitch_cents = 37;
    a.pan = -30;
    WlMixVoiceDesc b = sample_desc(2, mono, 3000, false, WL_BUS_SPEECH);
    b.pan = 64;
    b.gain = 300;
    b.rate = 22050;
    WlMixVoiceDesc c = sample_desc(3, mono, 3000, false, WL_BUS_UI);
    c.pitch_cents = -700;
    c.fade_in_frames = 700;
    wl_mixer_voice_start(&g_m, &a);
    static int16_t out[2 * 256 * 40];
    for (int blk = 0; blk < 40; blk++) {
        if (blk == 3) {
            wl_mixer_voice_start(&g_m, &b);
        }
        if (blk == 6) {
            wl_mixer_voice_start(&g_m, &c);
        }
        if (blk == 20) {
            wl_mixer_voice_stop(&g_m, 1, 2000);
            wl_mixer_bus_set(&g_m, WL_BUS_UI, 90);
        }
        wl_mixer_mix(&g_m, out + 512 * blk, 256);
    }
    static uint8_t bytes[sizeof out]; // hash explicit little-endian bytes: the same on any host
    for (size_t i = 0; i < sizeof out / 2; i++) {
        bytes[2 * i] = (uint8_t)((uint16_t)out[i] & 0xFFu);
        bytes[2 * i + 1] = (uint8_t)((uint16_t)out[i] >> 8);
    }
    const uint64_t h = wl_fnv1a64(bytes, sizeof bytes);
    ASSERT_EQ(0xe4445be3a6eb08caull, h); // intended output changes re-bless this value
    PASS();
}

SUITE(mixer_suite) {
    RUN_TEST(silence_without_voices);
    RUN_TEST(stereo_source_at_unity_is_the_identity);
    RUN_TEST(mono_pan_law_is_constant_power);
    RUN_TEST(stereo_balance_leaves_the_centre_alone);
    RUN_TEST(default_bus_gains_and_volume_controls);
    RUN_TEST(output_saturates_and_counts_clipping);
    RUN_TEST(loop_points_wrap_exactly);
    RUN_TEST(loop_wraps_at_a_pitched_step_without_drifting);
    RUN_TEST(pitch_steps_and_interpolation);
    RUN_TEST(fades_in_and_out);
    RUN_TEST(gain_changes_ramp_without_steps);
    RUN_TEST(voice_stealing_follows_priority_then_age);
    RUN_TEST(speech_ducks_the_music_bus);
    RUN_TEST(pull_source_unity_pitched_and_ended);
    RUN_TEST(stop_pcm_removes_voices_using_a_sample);
    RUN_TEST(partial_blocks_are_allowed);
    RUN_TEST(regression_hash_of_a_mixed_scene);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mixer_suite);
    GREATEST_MAIN_END();
}
