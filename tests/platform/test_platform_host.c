// SPDX-License-Identifier: MIT
// The host stubs of platform.h (system events, log ring, input, video, GX recording, audio
// output) and the hardware-free GX helpers (texture sizes, filter resolution).
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/host/audio_host.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/host/input_host.h"
#include "wl/platform/host/system_host.h"
#include "wl/platform/platform.h"

TEST system_events_latch_and_clear(void) {
    wl_platform_system_init();
    ASSERT_EQ(0u, wl_platform_system_events());
    wl_host_system_raise(WL_SYS_RESET);
    wl_host_system_raise(WL_SYS_POWER);
    ASSERT_EQ((uint32_t)(WL_SYS_RESET | WL_SYS_POWER), wl_platform_system_events());
    ASSERT_EQ(0u, wl_platform_system_events());
    uint64_t t0 = wl_platform_time_us();
    wl_platform_thread_sleep_ms(2);
    ASSERT(wl_platform_time_us() >= t0 + 1000u);
    PASS();
}

TEST log_ring_keeps_the_last_lines(void) {
    wl_platform_log_init(NULL);
    wl_log_set_level(WL_LOG_LEVEL_DEBUG);
    for (int i = 0; i < 40; i++) {
        WL_LOG_D("test", "line %d", i);
    }
    const char *lines[4];
    ASSERT_EQ(4u, wl_platform_log_tail(lines, 4));
    ASSERT_STR_EQ("[D][test] line 39", lines[3]);
    ASSERT_STR_EQ("[D][test] line 36", lines[0]);
    wl_platform_log_flush();
    wl_log_set_sink(NULL, NULL);
    PASS();
}

TEST input_returns_the_preloaded_state(void) {
    WlRawInput in;
    wl_platform_input_poll(&in);
    ASSERT(!in.remotes[0].connected && !in.pads[0].connected && !in.keyboard.connected);
    WlRawInput want;
    memset(&want, 0, sizeof want);
    want.remotes[1].connected = true;
    want.remotes[1].buttons = WL_WM_A | WL_NC_Z;
    want.remotes[1].extension = WL_EXT_NUNCHUK;
    want.pads[0].connected = true;
    want.pads[0].buttons = WL_PAD_START;
    wl_host_input_set(&want);
    wl_platform_input_poll(&in);
    ASSERT(in.remotes[1].connected);
    ASSERT_EQ(WL_WM_A | WL_NC_Z, in.remotes[1].buttons);
    ASSERT_EQ(WL_PAD_START, in.pads[0].buttons);
    wl_platform_rumble(2, true);
    wl_platform_pad_rumble(3, true);
    ASSERT(wl_host_remote_rumble(2) && !wl_host_remote_rumble(1));
    ASSERT(wl_host_pad_rumble(3));
    wl_platform_rumble(9, true); // out of range: ignored
    wl_platform_input_shutdown();
    PASS();
}

TEST tex_bytes_follow_the_tile_sizes(void) {
    ASSERT_EQ((size_t)32, wl_gx_tex_bytes(WL_GX_TEX_I4, 8, 8));     // one 8x8 tile of 4 bpp
    ASSERT_EQ((size_t)64, wl_gx_tex_bytes(WL_GX_TEX_I4, 9, 8));     // rounds up to two tiles
    ASSERT_EQ((size_t)32, wl_gx_tex_bytes(WL_GX_TEX_I8, 8, 4));
    ASSERT_EQ((size_t)64, wl_gx_tex_bytes(WL_GX_TEX_RGBA8, 4, 4));  // RGBA8 tiles are 64 bytes
    ASSERT_EQ((size_t)(64 * 64 * 2), wl_gx_tex_bytes(WL_GX_TEX_RGB5A3, 64, 64));
    ASSERT_EQ((size_t)(64 * 64 / 2), wl_gx_tex_bytes(WL_GX_TEX_CMPR, 64, 64));
    ASSERT_EQ((size_t)0, wl_gx_tex_bytes(WL_GX_TEX_FORMAT_COUNT, 8, 8));
    // 16x16 RGB565 with 5 levels: 512 + 128 + 32 + 32 (2x2 rounds up to a 4x4 tile) + 32
    ASSERT_EQ((size_t)736, wl_gx_tex_bytes_mips(WL_GX_TEX_RGB565, 16, 16, 5));
    ASSERT_EQ(wl_gx_tex_bytes(WL_GX_TEX_I8, 32, 32), wl_gx_tex_bytes_mips(WL_GX_TEX_I8, 32, 32, 0));
    PASS();
}

TEST filter_override_resolution(void) {
    uint8_t mn, mg;
    WlGxSampler s;
    memset(&s, 0, sizeof s);
    s.min_filter = WL_GX_FILTER_LINEAR_MIP_LINEAR;
    s.mag_filter = WL_GX_FILTER_LINEAR;
    wl_gx_resolve_filters(&s, 4, WL_GX_FILTER_OVERRIDE_NONE, &mn, &mg);
    ASSERT_EQ(WL_GX_FILTER_LINEAR_MIP_LINEAR, mn);
    wl_gx_resolve_filters(&s, 1, WL_GX_FILTER_OVERRIDE_NONE, &mn, &mg); // no mips: mip filter dropped
    ASSERT_EQ(WL_GX_FILTER_LINEAR, mn);
    wl_gx_resolve_filters(&s, 4, WL_GX_FILTER_OVERRIDE_NEAREST, &mn, &mg);
    ASSERT_EQ(WL_GX_FILTER_NEAREST_MIP_LINEAR, mn); // keeps the mip blend
    ASSERT_EQ(WL_GX_FILTER_NEAREST, mg);
    s.pinned = true;
    wl_gx_resolve_filters(&s, 4, WL_GX_FILTER_OVERRIDE_NEAREST, &mn, &mg);
    ASSERT_EQ(WL_GX_FILTER_LINEAR_MIP_LINEAR, mn); // pinned samplers ignore the override
    ASSERT_EQ(WL_GX_FILTER_LINEAR, mg);
    PASS();
}

TEST gx_stub_records_frames_and_draws(void) {
    WlVideoInfo vi;
    ASSERT_EQ(WL_OK, wl_platform_video_init());
    wl_platform_video_info(&vi);
    ASSERT_EQ(640, vi.fb_width);
    ASSERT_EQ(WL_OK, wl_gx_init());
    WlHostGxDraw rec[4];
    wl_host_gx_record(rec, 4);
    wl_gx_begin_frame(0x102030FFu);
    wl_gx_set_recipe(WL_GX_RECIPE_KONST);
    wl_gx_set_konst(0x11223344u);
    const WlGxVertex quad[4] = {{10, 20, 0, 0xFFFFFFFFu, 0, 0},
                                {30, 20, 0, 0xFFFFFFFFu, 1, 0},
                                {30, 60, 0, 0xFFFFFFFFu, 1, 1},
                                {10, 60, 0, 0xFFFFFFFFu, 0, 1}};
    wl_gx_draw(WL_GX_QUADS, quad, 4);
    wl_gx_end_frame();
    ASSERT_EQ(1u, wl_host_gx_recorded());
    ASSERT_EQ(WL_GX_RECIPE_KONST, rec[0].recipe);
    ASSERT_EQ(0x11223344u, rec[0].konst);
    ASSERT(rec[0].x0 == 10.0f && rec[0].y1 == 60.0f);
    WlGxStats st;
    wl_gx_stats(&st);
    ASSERT_EQ(1u, st.frames);
    ASSERT_EQ(1u, st.draw_calls);
    ASSERT_EQ(4u, st.vertices);
    ASSERT_EQ(0u, st.validation_errors);
    ASSERT_EQ(0x102030FFu, wl_host_gx_state()->clear_rgba);
    wl_host_gx_record(NULL, 0);
    wl_gx_shutdown();
    PASS();
}

static uint32_t g_fill_calls;

static void fill_ramp(void *user, int16_t *stereo, uint32_t frames) {
    uint32_t *next = user;
    for (uint32_t i = 0; i < frames; i++) {
        stereo[2 * i] = (int16_t)(*next & 0x7FFF);
        stereo[2 * i + 1] = (int16_t)-(int16_t)(*next & 0x7FFF);
        (*next)++;
    }
    g_fill_calls++;
}

TEST audio_stub_renders_through_the_fill_callback(void) {
    uint32_t next = 0;
    ASSERT_EQ(WL_ERR_ARG, wl_platform_audio_start(32000, 256, NULL, NULL));
    ASSERT_EQ(WL_OK, wl_platform_audio_start(32000, 256, fill_ramp, &next));
    ASSERT_EQ(WL_ERR_STATE, wl_platform_audio_start(32000, 256, fill_ramp, &next));
    ASSERT(!wl_platform_audio_threaded());
    static int16_t out[2 * 512];
    ASSERT_EQ(512u, wl_host_audio_render(out, 300)); // rounded up to whole blocks
    ASSERT_EQ(2u, g_fill_calls);
    ASSERT_EQ(511, out[2 * 511]);
    ASSERT_EQ(-511, out[2 * 511 + 1]);
    wl_host_audio_inject_underruns(3);
    WlPlatformAudioStats st;
    wl_platform_audio_stats(&st);
    ASSERT_EQ(3u, st.backend);
    ASSERT_EQ(256u, st.hw_block_frames);
    ASSERT_EQ(2u, st.blocks_filled);
    ASSERT_EQ(3u, wl_platform_audio_underruns());
    wl_platform_audio_stop();
    ASSERT_EQ(0u, wl_host_audio_render(out, 256));
    PASS();
}

SUITE(host_suite) {
    RUN_TEST(system_events_latch_and_clear);
    RUN_TEST(log_ring_keeps_the_last_lines);
    RUN_TEST(input_returns_the_preloaded_state);
    RUN_TEST(tex_bytes_follow_the_tile_sizes);
    RUN_TEST(filter_override_resolution);
    RUN_TEST(gx_stub_records_frames_and_draws);
    RUN_TEST(audio_stub_renders_through_the_fill_callback);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(host_suite);
    GREATEST_MAIN_END();
}
