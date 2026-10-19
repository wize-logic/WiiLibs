// SPDX-License-Identifier: MIT
// The render targets (wl/platform/gx_target.h) on the host stub: who owns the EFB, when the back
// buffer is parked and put back, when a target's pixels are copied out, the clear quads and the
// allocation of the parked blocks, checked against the operations the stub records. And the ramp
// (platform.h): its image, its coordinates and its bind.
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/gx_direct.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/host/gx_renderer_host.h"
#include "wl/platform/host/gx_target_host.h"

#define BACK_W 640u
#define BACK_H 480u

static uint32_t g_allocs, g_frees;
static bool g_alloc_fails;
static size_t g_last_alloc_bytes;

static void *counting_alloc(size_t bytes) {
    if (g_alloc_fails) {
        return NULL;
    }
    g_allocs++;
    g_last_alloc_bytes = bytes;
    return aligned_alloc(32, bytes);
}

static void counting_free(void *p) {
    g_frees++;
    free(p);
}

// An allocator that has nothing for its second request.
static uint32_t g_alloc_calls;

static void *second_alloc_fails(size_t bytes) {
    return ++g_alloc_calls == 2u ? NULL : counting_alloc(bytes);
}

static void start(void) {
    g_allocs = g_frees = 0;
    g_alloc_fails = false;
    wl_gx_target_set_allocator(counting_alloc, counting_free);
    wl_gx_target_set_back_buffer_size(BACK_W, BACK_H);
    (void)wl_gx_init();
    wl_host_target_clear_ops();
    WlGxTargetStats ignored;
    wl_gx_target_stats(&ignored, true);
}

static void finish(void) {
    wl_gx_shutdown();
}

static uint8_t g_memory_a[64 * 64 * 4] __attribute__((aligned(32)));
static uint8_t g_memory_b[32 * 32 * 2] __attribute__((aligned(32)));
static const int g_id_a = 1, g_id_b = 2;

static WlGxTarget target_a(void) {
    WlGxTarget t = {&g_id_a, g_memory_a, 64, 64, WL_GX_TEX_RGBA8, 0xA, false};
    return t;
}

static WlGxTarget target_b(void) {
    WlGxTarget t = {&g_id_b, g_memory_b, 32, 32, WL_GX_TEX_RGB565, 0xB, false};
    return t;
}

static const WlHostTargetOp *ops(uint32_t *count) {
    return wl_host_target_ops(count);
}

TEST resolvable_and_sizes(void) {
    WlGxTarget a = target_a();
    ASSERT(wl_gx_target_resolvable(&a, BACK_W, BACK_H));
    ASSERT_EQ((size_t)(64 * 64 * 4), wl_gx_target_bytes(&a));
    WlGxTarget b = target_b();
    ASSERT(wl_gx_target_resolvable(&b, BACK_W, BACK_H));
    ASSERT_EQ((size_t)(32 * 32 * 2), wl_gx_target_bytes(&b));

    WlGxTarget t = a;
    t.memory = NULL;
    ASSERT(!wl_gx_target_resolvable(&t, BACK_W, BACK_H)); // nowhere to copy to
    t = a;
    t.memory = g_memory_a + 4;
    ASSERT(!wl_gx_target_resolvable(&t, BACK_W, BACK_H)); // the copy needs 32-byte alignment
    t = a;
    t.format = WL_GX_TEX_I8;
    ASSERT(!wl_gx_target_resolvable(&t, BACK_W, BACK_H)); // a format a copy has none of
    ASSERT_EQ((size_t)0, wl_gx_target_bytes(&t));
    t = a;
    t.width = 66;
    ASSERT(!wl_gx_target_resolvable(&t, BACK_W, BACK_H)); // not a multiple of 4
    t = a;
    t.height = 0;
    ASSERT(!wl_gx_target_resolvable(&t, BACK_W, BACK_H));
    t = a;
    t.width = 1024;
    ASSERT(!wl_gx_target_resolvable(&t, BACK_W, BACK_H)); // wider than the back buffer
    t.width = 640;
    t.height = 480;
    ASSERT(wl_gx_target_resolvable(&t, BACK_W, BACK_H)); // as large as it
    ASSERT(!wl_gx_target_resolvable(NULL, BACK_W, BACK_H));

    uint32_t w, h;
    t = a;
    t.width = 1024;
    t.height = 256;
    wl_gx_target_scissor_size(&t, BACK_W, BACK_H, &w, &h);
    ASSERT_EQ(BACK_W, w);
    ASSERT_EQ(256u, h);
    PASS();
}

TEST back_buffer_parked_when_drawn_then_target_resolved_and_back_restored(void) {
    start();
    wl_gx_begin_frame(0x000000FFu);
    WlGxTarget a = target_a();
    WlGxTarget b = target_b();
    uint32_t n;

    // nothing drawn: the back buffer is not parked, the target just gets its scissor
    ASSERT_EQ(NULL, wl_gx_target_use(&a));
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(1u, n);
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[0].kind);
    ASSERT_EQ(64u, o[0].w);
    ASSERT_EQ(64u, o[0].h);
    ASSERT(wl_gx_target_current() != &a); // (a copy of the descriptor is held)
    ASSERT(wl_gx_target_current() != NULL && wl_gx_target_current()->id == &g_id_a);

    // a target resolved on its way out, the next one scissored
    wl_host_target_clear_ops();
    const WlGxTarget *done = wl_gx_target_use(&b);
    ASSERT(done != NULL);
    ASSERT_EQ(&g_id_a, done->id);
    ASSERT_EQ(g_memory_a, done->memory);
    ASSERT_EQ((uintptr_t)0xA, done->user);
    o = ops(&n);
    ASSERT_EQ(4u, n);
    ASSERT_EQ(WL_HOST_TARGET_COPY_BEGIN, o[0].kind);
    ASSERT_EQ(WL_HOST_TARGET_COPY, o[1].kind);
    ASSERT_EQ(WL_GX_TARGET_COPY_RGBA8, o[1].what);
    ASSERT_EQ(g_memory_a, o[1].dst);
    ASSERT_EQ(64u, o[1].w);
    ASSERT_EQ(64u, o[1].h);
    ASSERT_EQ(WL_HOST_TARGET_COPY_END, o[2].kind);
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[3].kind);
    ASSERT_EQ(32u, o[3].w);

    // the back buffer back with nothing parked: only the scissor of all of it
    wl_host_target_clear_ops();
    done = wl_gx_target_use(NULL);
    ASSERT(done != NULL);
    ASSERT_EQ(&g_id_b, done->id);
    o = ops(&n);
    ASSERT_EQ(4u, n);
    ASSERT_EQ(WL_HOST_TARGET_COPY, o[1].kind);
    ASSERT_EQ(WL_GX_TARGET_COPY_RGB565, o[1].what);
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[3].kind);
    ASSERT_EQ(BACK_W, o[3].w);
    ASSERT_EQ(BACK_H, o[3].h);
    ASSERT(wl_gx_target_current() == NULL);

    // something drawn into the back buffer: parked on the way out (as much as the target can draw
    // over: its scissor), put back on the way in
    wl_gx_target_drawn();
    wl_host_target_clear_ops();
    ASSERT_EQ(NULL, wl_gx_target_use(&a));
    o = ops(&n);
    ASSERT_EQ(5u, n);
    ASSERT_EQ(WL_HOST_TARGET_COPY_BEGIN, o[0].kind);
    ASSERT_EQ(WL_HOST_TARGET_COPY, o[1].kind);
    ASSERT_EQ(WL_GX_TARGET_COPY_RGBA8, o[1].what);
    ASSERT_EQ(64u, o[1].w);
    ASSERT_EQ(64u, o[1].h);
    const void *parked_colour = o[1].dst;
    ASSERT_EQ(WL_HOST_TARGET_COPY, o[2].kind);
    ASSERT_EQ(WL_GX_TARGET_COPY_Z24X8, o[2].what);
    ASSERT_EQ(64u, o[2].w);
    ASSERT_EQ(64u, o[2].h);
    const void *parked_depth = o[2].dst;
    ASSERT(parked_colour != parked_depth);
    ASSERT_EQ(WL_HOST_TARGET_COPY_END, o[3].kind);
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[4].kind);
    ASSERT_EQ(2u, g_allocs);
    ASSERT_EQ((size_t)(64 * 64 * 4), g_last_alloc_bytes); // (blocks of the part parked)

    wl_host_target_clear_ops();
    done = wl_gx_target_use(NULL);
    ASSERT(done != NULL);
    o = ops(&n);
    ASSERT_EQ(5u, n); // a's copy, the scissor, the restore
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[3].kind);
    ASSERT_EQ(BACK_W, o[3].w);
    ASSERT_EQ(WL_HOST_TARGET_RESTORE, o[4].kind);
    ASSERT_EQ(parked_colour, o[4].dst);
    ASSERT_EQ(parked_depth, o[4].depth_src);
    ASSERT_EQ(64u, o[4].w); // the part parked...
    ASSERT_EQ(64u, o[4].h);
    ASSERT_EQ(BACK_W, o[4].back_w); // ...of the whole back buffer
    ASSERT_EQ(BACK_H, o[4].back_h);

    // the parked blocks are kept for the next park (a smaller part), and the restore did not park again
    wl_gx_target_drawn();
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(&b);
    ASSERT_EQ(2u, g_allocs);
    o = ops(&n);
    ASSERT_EQ(parked_colour, o[1].dst);
    ASSERT_EQ(32u, o[1].w);
    ASSERT_EQ(32u, o[1].h);

    WlGxTargetStats st;
    wl_gx_target_stats(&st, false);
    ASSERT_EQ(2u, st.parks);
    ASSERT_EQ(1u, st.restores);
    ASSERT_EQ(3u, st.resolves);
    ASSERT_EQ(0u, st.skipped);
    finish();
    ASSERT_EQ(2u, g_frees); // given back at shutdown
    PASS();
}

TEST same_target_is_not_a_change(void) {
    start();
    wl_gx_begin_frame(0);
    WlGxTarget a = target_a();
    (void)wl_gx_target_use(&a);
    wl_host_target_clear_ops();
    a.user = 0xC; // the descriptor as the caller has it now
    ASSERT_EQ(NULL, wl_gx_target_use(&a));
    uint32_t n;
    (void)ops(&n);
    ASSERT_EQ(0u, n);
    ASSERT_EQ((uintptr_t)0xC, wl_gx_target_current()->user);
    // the back buffer asked for while it has the EFB: no change either
    (void)wl_gx_target_use(NULL);
    wl_host_target_clear_ops();
    ASSERT_EQ(NULL, wl_gx_target_use(NULL));
    (void)ops(&n);
    ASSERT_EQ(0u, n);
    finish();
    PASS();
}

TEST drawn_is_recorded_for_the_back_buffer_only(void) {
    start();
    wl_gx_begin_frame(0);
    WlGxTarget a = target_a();
    (void)wl_gx_target_use(&a);
    wl_gx_target_drawn(); // into the target: not the back buffer's
    (void)wl_gx_target_use(NULL);
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(&a); // the back buffer was never drawn: not parked
    uint32_t n;
    (void)ops(&n);
    ASSERT_EQ(1u, n);
    ASSERT_EQ(0u, g_allocs);
    finish();
    PASS();
}

TEST a_target_that_cannot_be_copied_is_skipped_and_still_drawn(void) {
    start();
    wl_gx_begin_frame(0);
    WlGxTarget big = target_a();
    big.width = 1024;
    big.height = 256;
    uint32_t n;
    ASSERT_EQ(NULL, wl_gx_target_use(&big));
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(1u, n);
    ASSERT_EQ(BACK_W, o[0].w); // (the scissor no larger than the back buffer)
    ASSERT_EQ(256u, o[0].h);
    wl_host_target_clear_ops();
    ASSERT_EQ(NULL, wl_gx_target_use(NULL)); // nothing resolved
    o = ops(&n);
    ASSERT_EQ(1u, n); // the scissor only: no copy
    WlGxTargetStats st;
    wl_gx_target_stats(&st, true);
    ASSERT_EQ(1u, st.skipped);
    ASSERT_EQ(0u, st.resolves);
    wl_gx_target_stats(&st, false);
    ASSERT_EQ(0u, st.skipped); // (reset)
    finish();
    PASS();
}

TEST no_memory_to_park_leaves_the_back_buffer_and_asks_again(void) {
    start();
    wl_gx_begin_frame(0);
    wl_gx_target_drawn();
    g_alloc_fails = true;
    WlGxTarget a = target_a();
    uint32_t n;
    (void)wl_gx_target_use(&a);
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(1u, n); // no copies: only the scissor
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[0].kind);
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(NULL); // nothing parked: nothing put back
    o = ops(&n);
    ASSERT_EQ(4u, n); // a's copy, then the scissor
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[3].kind);

    g_alloc_fails = false;
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(&a);
    o = ops(&n);
    ASSERT_EQ(5u, n); // parked now
    ASSERT_EQ(2u, g_allocs);
    WlGxTargetStats st;
    wl_gx_target_stats(&st, false);
    ASSERT_EQ(1u, st.parks);
    finish();
    PASS();
}

TEST half_a_failed_allocation_is_given_back(void) {
    // the second block fails: the first is released
    start();
    wl_gx_begin_frame(0);
    wl_gx_target_drawn();
    g_allocs = 0;
    g_alloc_fails = false;
    g_alloc_calls = 0;
    wl_gx_target_set_allocator(second_alloc_fails, counting_free);
    WlGxTarget a = target_a();
    (void)wl_gx_target_use(&a);
    ASSERT_EQ(1u, g_allocs);
    ASSERT_EQ(1u, g_frees);
    finish();
    PASS();
}

TEST the_frame_resets_the_owner_and_the_end_puts_the_back_buffer_back(void) {
    start();
    wl_gx_begin_frame(0);
    wl_gx_target_drawn();
    WlGxTarget a = target_a();
    (void)wl_gx_target_use(&a);
    wl_host_target_clear_ops();
    wl_gx_end_frame(); // the back buffer has the EFB for the display copy
    uint32_t n;
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(5u, n); // a's copy, the scissor, the restore
    ASSERT_EQ(WL_HOST_TARGET_RESTORE, o[4].kind);
    ASSERT(wl_gx_target_current() == NULL);

    // a frame begun while a target held the EFB (the caller skipped the end): the back buffer is the owner
    (void)wl_gx_target_use(&a);
    wl_gx_begin_frame(0);
    ASSERT(wl_gx_target_current() == NULL);
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(&a);
    (void)ops(&n);
    ASSERT_EQ(1u, n); // not drawn this frame: not parked
    finish();
    PASS();
}

TEST clear_quads(void) {
    start();
    wl_gx_begin_frame(0);
    uint32_t n;
    wl_gx_target_clear(true, false, 0x11223344u, 0.25f, 8, 16, 100, 50);
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(1u, n);
    ASSERT_EQ(WL_HOST_TARGET_CLEAR, o[0].kind);
    ASSERT(o[0].colour && !o[0].depth);
    ASSERT_EQ(0x11223344u, o[0].rgba);
    ASSERT_EQ(8, o[0].x);
    ASSERT_EQ(16, o[0].y);
    ASSERT_EQ(100u, o[0].w);
    ASSERT_EQ(50u, o[0].h);
    ASSERT_EQ(BACK_W, o[0].back_w);
    ASSERT_EQ(BACK_H, o[0].back_h);
    ASSERT_EQ(0.25f, o[0].z);
    wl_gx_target_clear(false, true, 0, 7.0f, 0, 0, 1, 1); // the depth clamped
    wl_gx_target_clear(false, true, 0, -3.0f, 0, 0, 1, 1);
    o = ops(&n);
    ASSERT_EQ(3u, n);
    ASSERT_EQ(1.0f, o[1].z);
    ASSERT_EQ(0.0f, o[2].z);
    WlGxTargetStats st;
    wl_gx_target_stats(&st, false);
    ASSERT_EQ(3u, st.clears);
    finish();
    PASS();
}

TEST a_smaller_back_buffer_is_what_is_parked_and_restored(void) {
    start();
    wl_gx_target_set_back_buffer_size(320, 240);
    wl_gx_begin_frame(0);
    wl_gx_target_drawn();
    WlGxTarget a = target_a();
    a.width = 640; // (larger than the back buffer: its scissor, and the part parked, are all of it)
    a.height = 480;
    uint32_t n;
    (void)wl_gx_target_use(&a);
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(320u, o[1].w);
    ASSERT_EQ(240u, o[1].h);
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(NULL);
    o = ops(&n);
    ASSERT_EQ(2u, n); // (a not resolved: larger than the back buffer) the scissor, the restore
    ASSERT_EQ(WL_HOST_TARGET_RESTORE, o[1].kind);
    ASSERT_EQ(320u, o[1].w);
    ASSERT_EQ(240u, o[1].h);
    ASSERT_EQ(320u, o[1].back_w);
    wl_gx_target_clear(true, true, 0, 0.0f, 0, 0, 320, 240);
    o = ops(&n);
    ASSERT_EQ(320u, o[n - 1].back_w);
    ASSERT_EQ(240u, o[n - 1].back_h);
    finish();
    wl_gx_target_set_back_buffer_size(BACK_W, BACK_H);
    PASS();
}

TEST squeeze_narrows_the_finished_picture_with_the_parked_colour_block(void) {
    start();
    uint32_t n;
    ASSERT(!wl_gx_target_squeeze(80, 0));      // nothing to draw
    ASSERT(!wl_gx_target_squeeze(400, 300));   // past the back buffer's width
    ASSERT_EQ(0u, g_allocs);
    ASSERT(wl_gx_target_squeeze(80, 480));
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(1u, n);
    ASSERT_EQ(WL_HOST_TARGET_SQUEEZE, o[0].kind);
    ASSERT_EQ(80, o[0].x);
    ASSERT_EQ(480u, o[0].w);
    ASSERT_EQ(BACK_W, o[0].back_w);
    ASSERT_EQ(BACK_H, o[0].back_h);
    ASSERT(o[0].dst != NULL);
    ASSERT_EQ(1u, g_allocs); // one block, the colour's
    const void *block = o[0].dst;
    ASSERT(wl_gx_target_squeeze(80, 480));
    ASSERT_EQ(1u, g_allocs); // ...kept for the next frame
    o = ops(&n);
    ASSERT_EQ(block, o[1].dst);
    // a park after it takes the block it finds and asks for the depth's only
    WlGxTarget a = target_a();
    wl_gx_target_frame_begun(0x000000FFu);
    wl_gx_target_drawn();
    (void)wl_gx_target_use(&a);
    ASSERT_EQ(2u, g_allocs);
    o = ops(&n);
    bool copied_into_block = false;
    for (uint32_t i = 0; i < n; i++) {
        copied_into_block = copied_into_block || (o[i].kind == WL_HOST_TARGET_COPY && o[i].dst == block);
    }
    ASSERT(copied_into_block);
    WlGxTargetStats st;
    wl_gx_target_stats(&st, false);
    ASSERT_EQ(2u, st.squeezes);
    ASSERT_EQ(1u, st.parks);
    finish();
    ASSERT_EQ(2u, g_frees); // both blocks given back
    PASS();
}

TEST squeeze_without_memory_leaves_the_picture_alone(void) {
    start();
    g_alloc_fails = true;
    ASSERT(!wl_gx_target_squeeze(80, 480));
    ASSERT(!wl_gx_target_squeeze(80, 480)); // (told once; asks again)
    uint32_t n;
    (void)ops(&n);
    ASSERT_EQ(0u, n);
    g_alloc_fails = false;
    ASSERT(wl_gx_target_squeeze(80, 480));
    finish();
    PASS();
}

TEST the_host_end_frame_squeezes_only_a_pillarboxed_picture(void) {
    start();
    wl_host_gx_set_widescreen(true);
    wl_gx_begin_frame(0);
    wl_host_target_clear_ops();
    wl_gx_end_frame();
    uint32_t n;
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(1u, n);
    ASSERT_EQ(WL_HOST_TARGET_SQUEEZE, o[0].kind);
    ASSERT_EQ(80, o[0].x);
    ASSERT_EQ(480u, o[0].w);
    ASSERT_EQ(WL_OK, wl_platform_video_set_widescreen(WL_WIDESCREEN_STRETCH));
    wl_gx_begin_frame(0);
    wl_host_target_clear_ops();
    wl_gx_end_frame();
    (void)ops(&n);
    ASSERT_EQ(0u, n);
    ASSERT_EQ(WL_OK, wl_platform_video_set_widescreen(WL_WIDESCREEN_PILLARBOX));
    wl_host_gx_set_widescreen(false);
    wl_gx_begin_frame(0);
    wl_host_target_clear_ops();
    wl_gx_end_frame();
    (void)ops(&n);
    ASSERT_EQ(0u, n); // a 4:3 setting
    finish();
    PASS();
}

static uint8_t g_memory_wide[128 * 128 * 4] __attribute__((aligned(32)));
static const int g_id_wide = 3;

TEST a_picture_drawn_narrow_parks_its_columns_alone_and_blackens_the_bars(void) {
    start();
    wl_gx_target_set_picture(80, 480);
    uint32_t x = 0, w = 0, n, i, clears = 0;
    ASSERT(wl_gx_target_picture(&x, &w));
    ASSERT_EQ(80u, x);
    ASSERT_EQ(480u, w);
    wl_gx_begin_frame(0x000000FFu);
    wl_gx_target_drawn();
    WlGxTarget a = target_a(), wide = {&g_id_wide, g_memory_wide, 128, 128, WL_GX_TEX_RGBA8, 0xC, false};
    // 64 by 64 from the EFB's origin: over the left bar alone, nothing parked
    (void)wl_gx_target_use(&a);
    const WlHostTargetOp *o = ops(&n);
    for (i = 0; i < n; i++) {
        ASSERT(o[i].kind != WL_HOST_TARGET_COPY);
    }
    // 128 by 128: a resolved, then columns 80..128 parked (colour and depth)
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(&wide);
    o = ops(&n);
    uint32_t parked = 0;
    for (i = 0; i < n; i++) {
        if (o[i].kind == WL_HOST_TARGET_COPY && o[i].dst != g_memory_a) {
            ASSERT_EQ(80, o[i].x);
            ASSERT_EQ(48u, o[i].w);
            ASSERT_EQ(128u, o[i].h);
            parked++;
        }
    }
    ASSERT_EQ(2u, parked);
    ASSERT(wl_gx_target_parked_colour(&w, &n) != NULL);
    ASSERT_EQ(48u, w);
    // back: those columns put back, and at the end of the frame both bars black
    wl_host_target_clear_ops();
    wl_gx_target_frame_ending();
    o = ops(&n);
    bool restored = false;
    for (i = 0; i < n; i++) {
        if (o[i].kind == WL_HOST_TARGET_RESTORE) {
            restored = o[i].x == 80 && o[i].w == 48u && o[i].h == 128u;
        }
        if (o[i].kind == WL_HOST_TARGET_CLEAR) {
            ASSERT(o[i].colour && !o[i].depth && o[i].rgba == 0x000000FFu && o[i].w == 80u && o[i].h == BACK_H);
            ASSERT(o[i].x == 0 || o[i].x == 560);
            clears++;
        }
    }
    ASSERT(restored);
    ASSERT_EQ(2u, clears);
    WlGxTargetStats st;
    wl_gx_target_stats(&st, false);
    ASSERT_EQ(1u, st.parks);
    ASSERT_EQ(1u, st.bars_parks);
    ASSERT_EQ(1u, st.bar_clears);
    ASSERT_EQ((uint64_t)wl_gx_tex_bytes(WL_GX_TEX_RGBA8, 48, 128), st.park_bytes);
    // a frame with nothing over the bars leaves them alone
    wl_gx_target_frame_begun(0x000000FFu);
    wl_host_target_clear_ops();
    wl_gx_target_frame_ending();
    (void)ops(&n);
    ASSERT_EQ(0u, n);
    wl_gx_target_set_picture(0, 0);
    ASSERT(!wl_gx_target_picture(&x, &w));
    finish();
    PASS();
}

static uint32_t g_disp[4]; // the display rectangle a renderer was told: src_x, src_w, dst_x, calls

static void disp_rect(void *user, uint32_t src_x, uint32_t src_w, uint32_t dst_x) {
    (void)user;
    g_disp[0] = src_x;
    g_disp[1] = src_w;
    g_disp[2] = dst_x;
    g_disp[3]++;
}

TEST a_picture_in_the_last_columns_leaves_the_first_to_targets_and_shows_its_own_alone(void) {
    start();
    WlHostGxRenderer r;
    memset(&r, 0, sizeof r);
    r.display_rect = disp_rect;
    memset(g_disp, 0, sizeof g_disp);
    wl_host_gx_set_renderer(&r);
    wl_host_gx_set_widescreen(true);
    ASSERT(wl_gx_draw_narrow(true));
    uint32_t x = 0, w = 0, n, i;
    // (16:9's 480 columns at the EFB's 160..640, shown at the XFB's 80..560)
    ASSERT(wl_gx_target_picture(&x, &w));
    ASSERT_EQ(160u, x);
    ASSERT_EQ(480u, w);
    ASSERT_EQ(160u, g_disp[0]);
    ASSERT_EQ(480u, g_disp[1]);
    ASSERT_EQ(80u, g_disp[2]);
    wl_gx_begin_frame(0x000000FFu);
    wl_gx_target_drawn();
    WlGxTarget wide = {&g_id_wide, g_memory_wide, 128, 128, WL_GX_TEX_RGBA8, 0xC, false};
    WlGxTarget a = target_a();
    // 128 by 128 from the EFB's origin, then 64 by 64: neither parks, the first is resolved
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(&wide);
    ASSERT(wl_gx_target_use(&a) != NULL);
    const WlHostTargetOp *o = ops(&n);
    uint32_t copies = 0;
    for (i = 0; i < n; i++) {
        if (o[i].kind == WL_HOST_TARGET_COPY) {
            ASSERT(o[i].dst == g_memory_wide);
            copies++;
        }
    }
    ASSERT_EQ(1u, copies);
    // back: nothing put back, and the columns beside the picture (not shown) never blackened
    wl_host_target_clear_ops();
    (void)wl_gx_end_frame();
    o = ops(&n);
    for (i = 0; i < n; i++) {
        ASSERT(o[i].kind != WL_HOST_TARGET_RESTORE && o[i].kind != WL_HOST_TARGET_CLEAR);
    }
    WlGxTargetStats st;
    wl_gx_target_stats(&st, false);
    ASSERT_EQ(0u, st.parks);
    ASSERT_EQ(0u, st.restores);
    ASSERT_EQ(0u, st.bar_clears);
    ASSERT_EQ(0u, (unsigned)st.park_bytes);
    // the switch narrowmid: in the middle columns, the display copy whole
    WlGxBisect b = *wl_gx_bisect();
    b.narrow_mid = true;
    wl_gx_bisect_set(&b);
    ASSERT(wl_gx_draw_narrow(true));
    ASSERT(wl_gx_target_picture(&x, &w));
    ASSERT_EQ(80u, x);
    ASSERT_EQ(0u, g_disp[1]);
    b.narrow_mid = false;
    wl_gx_bisect_set(&b);
    (void)wl_gx_draw_narrow(false);
    wl_host_gx_set_widescreen(false);
    wl_host_gx_set_renderer(NULL);
    wl_gx_target_set_bars_shown(true);
    finish();
    PASS();
}

TEST a_larger_target_parks_the_larger_part_after_putting_the_back_buffer_back(void) {
    start();
    wl_gx_begin_frame(0);
    wl_gx_target_drawn();
    WlGxTarget a = target_a(), b = target_b();
    uint32_t n;
    (void)wl_gx_target_use(&b); // 32 by 32 parked
    ASSERT_EQ(2u, g_allocs);
    wl_host_target_clear_ops();
    const WlGxTarget *done = wl_gx_target_use(&a); // 64 by 64: b resolved, the back buffer back, 64 parked
    ASSERT(done != NULL && done->id == &g_id_b);
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(10u, n);
    ASSERT_EQ(WL_HOST_TARGET_COPY, o[1].kind); // b's copy
    ASSERT_EQ(&g_memory_b[0], o[1].dst);
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[3].kind); // the back buffer's, and its 32 by 32 put back
    ASSERT_EQ(BACK_W, o[3].w);
    ASSERT_EQ(WL_HOST_TARGET_RESTORE, o[4].kind);
    ASSERT_EQ(32u, o[4].w);
    ASSERT_EQ(WL_HOST_TARGET_COPY, o[6].kind); // then 64 by 64 parked, into larger blocks
    ASSERT_EQ(WL_GX_TARGET_COPY_RGBA8, o[6].what);
    ASSERT_EQ(64u, o[6].w);
    ASSERT_EQ(64u, o[6].h);
    ASSERT_EQ(WL_GX_TARGET_COPY_Z24X8, o[7].what);
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[9].kind); // a's
    ASSERT_EQ(64u, o[9].w);
    ASSERT_EQ(4u, g_allocs);
    ASSERT_EQ(2u, g_frees);
    ASSERT_EQ((size_t)(64 * 64 * 4), g_last_alloc_bytes);

    // a smaller one after it: a resolved, nothing parked again
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(&b);
    o = ops(&n);
    ASSERT_EQ(4u, n);
    ASSERT_EQ(&g_memory_a[0], o[1].dst);
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(NULL); // the 64 by 64 put back
    o = ops(&n);
    ASSERT_EQ(WL_HOST_TARGET_RESTORE, o[4].kind);
    ASSERT_EQ(64u, o[4].w);
    ASSERT_EQ(64u, o[4].h);
    WlGxTargetStats st;
    wl_gx_target_stats(&st, false);
    ASSERT_EQ(2u, st.parks);
    ASSERT_EQ(2u, st.restores);
    finish();
    ASSERT_EQ(4u, g_frees);
    PASS();
}

TEST the_part_parked_is_whole_tiles_within_the_back_buffer(void) {
    start();
    wl_gx_begin_frame(0);
    wl_gx_target_drawn();
    WlGxTarget odd = target_a();
    odd.width = 66; // (not resolved, but drawn: its scissor is 66 by 30)
    odd.height = 30;
    uint32_t n;
    (void)wl_gx_target_use(&odd);
    const WlHostTargetOp *o = ops(&n);
    ASSERT_EQ(68u, o[1].w);
    ASSERT_EQ(32u, o[1].h);
    ASSERT_EQ(WL_HOST_TARGET_SCISSOR, o[4].kind);
    ASSERT_EQ(66u, o[4].w);
    ASSERT_EQ(30u, o[4].h);
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(NULL);
    WlGxTarget wide = target_a();
    wide.width = 1024; // (its scissor is the back buffer's width)
    wl_gx_target_drawn();
    wl_host_target_clear_ops();
    (void)wl_gx_target_use(&wide);
    o = ops(&n);
    ASSERT_EQ(BACK_W, o[1].w);
    ASSERT_EQ(64u, o[1].h);
    finish();
    PASS();
}

SUITE(target_suite) {
    RUN_TEST(resolvable_and_sizes);
    RUN_TEST(back_buffer_parked_when_drawn_then_target_resolved_and_back_restored);
    RUN_TEST(same_target_is_not_a_change);
    RUN_TEST(drawn_is_recorded_for_the_back_buffer_only);
    RUN_TEST(a_target_that_cannot_be_copied_is_skipped_and_still_drawn);
    RUN_TEST(no_memory_to_park_leaves_the_back_buffer_and_asks_again);
    RUN_TEST(half_a_failed_allocation_is_given_back);
    RUN_TEST(the_frame_resets_the_owner_and_the_end_puts_the_back_buffer_back);
    RUN_TEST(clear_quads);
    RUN_TEST(a_smaller_back_buffer_is_what_is_parked_and_restored);
    RUN_TEST(squeeze_narrows_the_finished_picture_with_the_parked_colour_block);
    RUN_TEST(squeeze_without_memory_leaves_the_picture_alone);
    RUN_TEST(the_host_end_frame_squeezes_only_a_pillarboxed_picture);
    RUN_TEST(a_larger_target_parks_the_larger_part_after_putting_the_back_buffer_back);
    RUN_TEST(the_part_parked_is_whole_tiles_within_the_back_buffer);
    RUN_TEST(a_picture_drawn_narrow_parks_its_columns_alone_and_blackens_the_bars);
    RUN_TEST(a_picture_in_the_last_columns_leaves_the_first_to_targets_and_shows_its_own_alone);
}

// ---- the ramp --------------------------------------------------------------------------------------

TEST ramp_texel_value_is_its_x_coordinate_in_tiled_order(void) {
    uint8_t image[WL_GX_RAMP_BYTES];
    memset(image, 0xEE, sizeof image);
    wl_gx_ramp_fill(image);
    ASSERT_EQ((size_t)WL_GX_RAMP_BYTES, wl_gx_tex_bytes(WL_GX_TEX_I8, WL_GX_RAMP_WIDTH, WL_GX_RAMP_HEIGHT));
    // I8 tiles are 8 wide and 4 high, 32 bytes, a row of 8 after another
    for (uint32_t y = 0; y < WL_GX_RAMP_HEIGHT; y++) {
        for (uint32_t x = 0; x < WL_GX_RAMP_WIDTH; x++) {
            uint32_t tile = x / 8u;
            ASSERT_EQ((int)(uint8_t)x, (int)image[tile * 32u + y * 8u + x % 8u]);
        }
    }
    // every byte written once: the 32 tiles hold each value four times
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (uint32_t i = 0; i < sizeof image; i++) {
        seen[image[i]]++;
    }
    for (uint32_t v = 0; v < 256; v++) {
        ASSERT_EQ(4u, seen[v]);
    }
    PASS();
}

TEST ramp_image_is_shared_aligned_and_the_same_as_the_fill(void) {
    const void *a = wl_gx_ramp_image();
    ASSERT(a != NULL);
    ASSERT_EQ(0u, (unsigned)((uintptr_t)a % 32u));
    ASSERT_EQ(a, wl_gx_ramp_image());
    uint8_t image[WL_GX_RAMP_BYTES];
    wl_gx_ramp_fill(image);
    ASSERT_EQ(0, memcmp(a, image, sizeof image));
    PASS();
}

TEST ramp_coordinate_reads_the_middle_of_the_texel(void) {
    ASSERT_EQ(0.5f / 256.0f, wl_gx_ramp_coordinate(0.0f));
    ASSERT_EQ(255.5f / 256.0f, wl_gx_ramp_coordinate(1.0f));
    ASSERT_EQ(wl_gx_ramp_coordinate(0.0f), wl_gx_ramp_coordinate(-4.0f));
    ASSERT_EQ(wl_gx_ramp_coordinate(1.0f), wl_gx_ramp_coordinate(9.0f));
    ASSERT_EQ(wl_gx_ramp_coordinate(0.0f), wl_gx_ramp_coordinate(0.0f / 0.0f)); // NaN
    // the texel a nearest lookup of s reads is floor(s * 256): the value, rounded to a byte
    for (uint32_t i = 0; i <= 255; i++) {
        float value = (float)i / 255.0f;
        ASSERT_EQ((int)i, (int)(wl_gx_ramp_coordinate(value) * 256.0f));
    }
    // one that does not land on a byte rounds to the nearest
    ASSERT_EQ(128, (int)(wl_gx_ramp_coordinate(0.5f) * 256.0f)); // 127.5 + 0.5
    PASS();
}

TEST ramp_bind_is_recorded(void) {
    start();
    wl_host_gx_clear_counters();
    wl_gx_ramp_bind(7);
    ASSERT_EQ(1u, wl_host_gx_state()->ramp_binds);
    ASSERT_EQ(7u, wl_host_gx_state()->ramp_texmap);
    finish();
    PASS();
}

SUITE(ramp_suite) {
    RUN_TEST(ramp_texel_value_is_its_x_coordinate_in_tiled_order);
    RUN_TEST(ramp_image_is_shared_aligned_and_the_same_as_the_fill);
    RUN_TEST(ramp_coordinate_reads_the_middle_of_the_texel);
    RUN_TEST(ramp_bind_is_recorded);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(target_suite);
    RUN_SUITE(ramp_suite);
    GREATEST_MAIN_END();
}
