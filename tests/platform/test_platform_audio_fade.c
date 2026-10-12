// SPDX-License-Identifier: MIT
// The audio output's master fade (wl_platform_audio_fade, wl/platform/audio_gain.c) on the host
// backend: untouched at gain 1, a linear ramp over the milliseconds asked, at once for 0 ms, a new
// fade continuing from where the ramp is, clamping, and the ramp counted in output frames.
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/host/audio_host.h"
#include "wl/platform/platform.h"

#define RATE 32000u
#define BLOCK 256u
#define LEVEL 10000

// a constant signal, so that the output is the gain times it
static void fill(void *user, int16_t *stereo, uint32_t frames) {
    (void)user;
    for (uint32_t i = 0; i < frames * 2u; i++) {
        stereo[i] = LEVEL;
    }
}

static int16_t g_out[BLOCK * 64u * 2u];

// renders `blocks` blocks and returns the first frame's left sample of the last block
static int render(uint32_t blocks) {
    int16_t *at = g_out;
    for (uint32_t b = 0; b < blocks; b++) {
        wl_host_audio_render(at, BLOCK);
        at += BLOCK * 2u;
    }
    return at[-(int)(BLOCK * 2u)];
}

static void start(void) {
    wl_platform_audio_stop();
    wl_platform_audio_fade(1.0f, 0);
    (void)wl_platform_audio_start(RATE, BLOCK, fill, NULL);
    wl_host_audio_render(NULL, BLOCK); // (the output sees the gain of 1 before the case asks for more)
}

TEST untouched_at_gain_one(void) {
    start();
    wl_host_audio_render(g_out, BLOCK);
    for (uint32_t i = 0; i < BLOCK * 2u; i++) {
        ASSERT_EQ(LEVEL, g_out[i]);
    }
    ASSERT(wl_platform_audio_gain() == 1.0f);
    PASS();
}

TEST zero_milliseconds_is_at_once(void) {
    start();
    wl_platform_audio_fade(0.0f, 0);
    wl_host_audio_render(g_out, BLOCK);
    for (uint32_t i = 0; i < BLOCK * 2u; i++) {
        ASSERT_EQ(0, g_out[i]);
    }
    wl_platform_audio_fade(0.5f, 0);
    wl_host_audio_render(g_out, BLOCK);
    ASSERT_EQ(LEVEL / 2, g_out[0]);
    ASSERT_EQ(LEVEL / 2, g_out[BLOCK * 2u - 1u]);
    PASS();
}

TEST a_fade_is_a_linear_ramp_over_its_milliseconds(void) {
    start();
    // 300 ms at 32 kHz is 9600 frames: 37.5 blocks; the ramp is linear and ends at the target
    wl_platform_audio_fade(0.0f, 300);
    wl_host_audio_render(g_out, BLOCK);
    ASSERT(g_out[0] < LEVEL && g_out[0] > LEVEL - 10);   // first frame: one step down
    ASSERT(g_out[BLOCK * 2u - 2u] < g_out[0]);            // falling
    // a third of the way (3200 frames = 12.5 blocks) the signal is about two thirds
    int16_t sample = (int16_t)render(12);
    ASSERT(sample > 6000 && sample < 7400);
    // past the end it is silent and stays so
    render(40);
    for (uint32_t i = 0; i < BLOCK * 2u; i++) {
        ASSERT_EQ(0, g_out[(39u * BLOCK * 2u) + i]);
    }
    ASSERT(wl_platform_audio_gain() == 0.0f);
    // and back up, the same way
    wl_platform_audio_fade(1.0f, 300);
    render(40);
    for (uint32_t i = 0; i < BLOCK * 2u; i++) {
        ASSERT_EQ(LEVEL, g_out[(39u * BLOCK * 2u) + i]);
    }
    // once at 1 and still, the block is not touched (the signal passes as it is)
    wl_host_audio_render(g_out, BLOCK);
    ASSERT_EQ(LEVEL, g_out[0]);
    PASS();
}

TEST a_new_fade_continues_from_where_the_ramp_is(void) {
    start();
    wl_platform_audio_fade(0.0f, 1000);
    render(16); // 4096 frames of 32000: 12.8% down
    int before = g_out[(15u * BLOCK * 2u) + BLOCK * 2u - 2u];
    wl_platform_audio_fade(1.0f, 100);
    wl_host_audio_render(g_out, BLOCK);
    // the first frame after is above the last before (heading up from there), not a jump to either end
    ASSERT(g_out[0] >= before - 2 && g_out[0] < LEVEL);
    PASS();
}

TEST the_target_is_clamped_and_nan_is_untouched(void) {
    start();
    wl_platform_audio_fade(2.0f, 0);
    ASSERT(wl_platform_audio_gain() == 1.0f);
    wl_platform_audio_fade(-1.0f, 0);
    ASSERT(wl_platform_audio_gain() == 0.0f);
    wl_platform_audio_fade(__builtin_nanf(""), 0);
    ASSERT(wl_platform_audio_gain() == 1.0f);
    PASS();
}

TEST the_fade_set_before_the_output_starts_applies_from_the_first_block(void) {
    wl_platform_audio_stop();
    wl_platform_audio_fade(0.0f, 0);
    ASSERT_EQ_FMT(WL_OK, wl_platform_audio_start(RATE, BLOCK, fill, NULL), "%d");
    wl_host_audio_render(g_out, BLOCK);
    ASSERT_EQ(0, g_out[0]);
    wl_platform_audio_stop();
    wl_platform_audio_fade(1.0f, 0);
    PASS();
}

SUITE(audio_fade_suite) {
    RUN_TEST(untouched_at_gain_one);
    RUN_TEST(zero_milliseconds_is_at_once);
    RUN_TEST(a_fade_is_a_linear_ramp_over_its_milliseconds);
    RUN_TEST(a_new_fade_continues_from_where_the_ramp_is);
    RUN_TEST(the_target_is_clamped_and_nan_is_untouched);
    RUN_TEST(the_fade_set_before_the_output_starts_applies_from_the_first_block);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(audio_fade_suite);
    GREATEST_MAIN_END();
}
