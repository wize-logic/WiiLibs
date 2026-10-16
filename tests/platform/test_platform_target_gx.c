// SPDX-License-Identifier: MIT
// The render targets' GPU half (wl/platform/wii/gx_target_wii.c) and their bookkeeping
// (gx_target.c) compiled against a recording stand-in for libogc's GX (tests/platform/gx_stub): the
// exact GX calls, in order, of parking the back buffer, resolving a target, putting the back buffer
// back through a Z texture, and a clear quad. These sequences are what a game that draws to
// textures on GX needs to come out the same on every run; the Wii build compiles the same files
// against the real header.
#include <ogc/gx.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/gx_target_internal.h"
#include "wl/platform/wii/gx_wii_internal.h"

// ---- what the files above call in the rest of the platform layer ---------------------------------

static unsigned g_commands;   // wl_gx_wii_note_commands calls
static unsigned g_shadow;     // wl_gx_invalidate_shadow calls

void wl_platform_video_info(WlVideoInfo *out) {
    memset(out, 0, sizeof *out);
    out->fb_width = 640;
    out->efb_height = 480;
    out->xfb_height = 480;
}

void wl_gpu_buffer_invalidate(void *ptr, size_t size) {
    gx_stub_logf("wl_gpu_buffer_invalidate %s %u", gx_stub_name(ptr), (unsigned)size);
}

void wl_gpu_buffer_commit(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
}

void wl_gx_invalidate_shadow(void) {
    g_shadow++;
    gx_stub_logf("wl_gx_invalidate_shadow");
}

void wl_gx_wii_copy_filter(bool display) {
    gx_stub_logf("copy_filter %s", display ? "display" : "off");
}

void wl_gx_wii_note_commands(void) {
    g_commands++;
}

void wl_gx_wii_note_efb_copy(void) {}

void wl_gx_wii_hang_event(char kind, uint32_t a, uint32_t b) {
    (void)kind;
    (void)a;
    (void)b;
}

bool wl_gx_sync(void) {
    return true; // (the stub GPU is never behind)
}

// ---- the test ---------------------------------------------------------------------------------------

static uint8_t g_parked_colour[640 * 480 * 4] __attribute__((aligned(32)));
static uint8_t g_parked_depth[640 * 480 * 4] __attribute__((aligned(32)));
static unsigned g_alloc_calls;

static void *park_alloc(size_t bytes) {
    (void)bytes;
    return g_alloc_calls++ == 0 ? (void *)g_parked_colour : (void *)g_parked_depth;
}

static void park_free(void *memory) {
    (void)memory;
}

static uint8_t g_memory_a[64 * 64 * 4] __attribute__((aligned(32)));
static uint8_t g_memory_b[32 * 32 * 2] __attribute__((aligned(32)));
static const int g_id_a = 1, g_id_b = 2;

static void start(void) {
    gx_stub_reset();
    gx_stub_clear_names();
    gx_stub_name_memory(g_parked_colour, "PC");
    gx_stub_name_memory(g_parked_depth, "PD");
    gx_stub_name_memory(g_memory_a, "A");
    gx_stub_name_memory(g_memory_b, "B");
    g_alloc_calls = 0;
    g_commands = g_shadow = 0;
    wl_gx_target_set_allocator(park_alloc, park_free);
    wl_gx_target_set_back_buffer_size(640, 480);
    wl_gx_target_set_lazy_depth(false);
    wl_gx_target_frame_begun();
}

// The lines of the log since `from` that start with `prefix`.
static unsigned log_count(unsigned from, const char *prefix) {
    unsigned n = 0;
    for (unsigned i = from; i < g_gx_stub_log_count; i++) {
        n += strncmp(g_gx_stub_log[i], prefix, strlen(prefix)) == 0;
    }
    return n;
}

// The log since `from` is exactly `n` lines, `lines`.
static enum greatest_test_res expect_log(unsigned from, const char *const *lines, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        ASSERT(from + i < g_gx_stub_log_count);
        ASSERT_STR_EQ(lines[i], g_gx_stub_log[from + i]);
    }
    ASSERT_EQ(from + n, g_gx_stub_log_count);
    PASS();
}

#define EXPECT(from, ...)                                                                          \
    do {                                                                                           \
        static const char *const expected_[] = {__VA_ARGS__};                                      \
        CHECK_CALL(expect_log((from), expected_, (unsigned)(sizeof expected_ / sizeof expected_[0]))); \
    } while (0)

static WlGxTarget target_a(void) {
    WlGxTarget t = {&g_id_a, g_memory_a, 64, 64, WL_GX_TEX_RGBA8, 0, false};
    return t;
}

static WlGxTarget target_b(void) {
    WlGxTarget t = {&g_id_b, g_memory_b, 32, 32, WL_GX_TEX_RGB565, 0, false};
    return t;
}

TEST park_the_back_buffer_and_take_the_efb(void) {
    start();
    WlGxTarget a = target_a();
    wl_gx_target_drawn();
    unsigned from = g_gx_stub_log_count;
    ASSERT(wl_gx_target_use(&a) == NULL);
    // colour then depth out of the EFB, unfiltered, as much as the target can draw over (64 x 64 from the
    // origin); the scissor of the target
    EXPECT(from, "copy_filter off",
           "wl_gpu_buffer_invalidate PC 16384", "GX_SetTexCopySrc 0 0 64 64", "GX_SetTexCopyDst 64 64 0x6 0",
           "GX_CopyTex PC 0", "GX_PixModeSync",
           "wl_gpu_buffer_invalidate PD 16384", "GX_SetTexCopySrc 0 0 64 64", "GX_SetTexCopyDst 64 64 0x16 0",
           "GX_CopyTex PD 0", "GX_PixModeSync",
           "copy_filter display", "GX_InvalidateTexAll",
           "GX_SetScissor 0 0 64 64", "wl_gx_invalidate_shadow");
    PASS();
}

TEST resolve_a_target_into_its_memory(void) {
    start();
    WlGxTarget a = target_a();
    WlGxTarget b = target_b();
    (void)wl_gx_target_use(&a);
    unsigned from = g_gx_stub_log_count;
    ASSERT(wl_gx_target_use(&b) != NULL); // (a, resolved)
    // a's pixels in GX's RGBA8 into its own memory (64 x 64 x 4 bytes), then b's scissor
    EXPECT(from, "copy_filter off",
           "wl_gpu_buffer_invalidate A 16384", "GX_SetTexCopySrc 0 0 64 64", "GX_SetTexCopyDst 64 64 0x6 0",
           "GX_CopyTex A 0", "GX_PixModeSync",
           "copy_filter display", "GX_InvalidateTexAll",
           "GX_SetScissor 0 0 32 32", "wl_gx_invalidate_shadow");
    from = g_gx_stub_log_count;
    ASSERT(wl_gx_target_use(NULL) != NULL);
    // b's, as RGB565 (32 x 32 x 2 bytes); the back buffer's scissor; nothing parked, nothing put back
    EXPECT(from, "copy_filter off",
           "wl_gpu_buffer_invalidate B 2048", "GX_SetTexCopySrc 0 0 32 32", "GX_SetTexCopyDst 32 32 0x4 0",
           "GX_CopyTex B 0", "GX_PixModeSync",
           "copy_filter display", "GX_InvalidateTexAll",
           "GX_SetScissor 0 0 640 480", "wl_gx_invalidate_shadow");
    PASS();
}

TEST put_the_back_buffer_back_through_a_z_texture(void) {
    start();
    WlGxTarget a = target_a();
    wl_gx_target_drawn();
    (void)wl_gx_target_use(&a);
    unsigned from = g_gx_stub_log_count;
    unsigned shadows = g_shadow;
    ASSERT(wl_gx_target_use(NULL) != NULL);
    EXPECT(from, "copy_filter off",
           "wl_gpu_buffer_invalidate A 16384", "GX_SetTexCopySrc 0 0 64 64", "GX_SetTexCopyDst 64 64 0x6 0",
           "GX_CopyTex A 0", "GX_PixModeSync",
           "copy_filter display", "GX_InvalidateTexAll",
           // the whole of the back buffer
           "GX_SetScissor 0 0 640 480", "wl_gx_invalidate_shadow",
           // the parked colour (64 x 64) as texture map 0 and the parked depth as map 1, unfiltered
           "GX_InvalidateTexAll",
           "GX_InitTexObj PC 64 64 0x6 0 0 0", "GX_InitTexObjFilterMode 0 0", "GX_LoadTexObj PC 0",
           "GX_InitTexObj PD 64 64 0x16 0 0 0", "GX_InitTexObjFilterMode 0 0", "GX_LoadTexObj PD 1",
           "GX_SetColorUpdate 1", "GX_SetAlphaUpdate 1", "GX_SetZMode 1 7 1",
           // the quad over the part parked: orthographic over the back buffer's pixels, depth from the Z
           // texture, replaced at the end
           "GX_LoadProjectionMtx ortho 0.003125 -1 -0.00416667 1 1 0 1", "GX_SetViewport 0 0 640 480 0 1",
           "GX_SetBlendMode 0 1 0 0", "GX_SetAlphaCompare 7 0 0 7 0", "GX_SetCullMode 0",
           "GX_SetNumIndStages 0", "GX_SetTevDirect 0", "GX_SetTevDirect 1", "GX_SetNumChans 0",
           "GX_SetNumTevStages 2", "GX_SetNumTexGens 1",
           "GX_SetTevOrder 0 0 0 255", "GX_SetTevSwapMode 0 0 0",
           "GX_SetTevColorIn 0 15 15 15 8", "GX_SetTevAlphaIn 0 7 7 7 4",
           "GX_SetTevColorOp 0 0 0 0 1 0", "GX_SetTevAlphaOp 0 0 0 0 1 0",
           "GX_SetTevOrder 1 0 1 255", "GX_SetTevSwapMode 1 0 0",
           "GX_SetTevColorIn 1 15 15 15 0", "GX_SetTevAlphaIn 1 7 7 7 0",
           "GX_SetTevColorOp 1 0 0 0 1 0", "GX_SetTevAlphaOp 1 0 0 0 1 0",
           "GX_SetZTexture 2 0x16 0", "GX_SetZCompLoc 0",
           "GX_LoadPosMtxImm identity 0", "GX_SetCurrentMtx 0", "GX_ClearVtxDesc",
           "GX_SetVtxDesc 9 1", "GX_SetVtxAttrFmt 0 9 1 4 0",
           "GX_SetVtxDesc 13 1", "GX_SetVtxAttrFmt 0 13 1 4 0", "GX_SetTexCoordGen 0 1 4 60",
           "GX_Begin 0x80 0 4",
           "GX_Position3f32 0 0 -1", "GX_TexCoord2f32 0 0",
           "GX_Position3f32 64 0 -1", "GX_TexCoord2f32 1 0",
           "GX_Position3f32 64 64 -1", "GX_TexCoord2f32 1 1",
           "GX_Position3f32 0 64 -1", "GX_TexCoord2f32 0 1",
           "GX_End", "GX_SetZTexture 0 0x11 0", "GX_SetZCompLoc 1", "wl_gx_invalidate_shadow");
    ASSERT(g_shadow > shadows);
    PASS();
}

TEST lazy_depth_parks_the_depth_only_before_a_target_writes_it(void) {
    start();
    wl_gx_target_set_lazy_depth(true);
    WlGxTarget a = target_a();
    WlGxTarget b = target_b();
    WlGxTargetStats stats;
    wl_gx_target_stats(&stats, true);
    // a target drawn without depth: the colour parked alone, put back alone (no Z texture, no depth
    // test or write)
    wl_gx_target_drawn();
    unsigned from = g_gx_stub_log_count;
    (void)wl_gx_target_use(&a);
    ASSERT_EQ(1u, log_count(from, "GX_CopyTex"));
    ASSERT_EQ(1u, log_count(from, "GX_CopyTex PC"));
    from = g_gx_stub_log_count;
    ASSERT(wl_gx_target_use(NULL) != NULL);
    ASSERT_EQ(1u, log_count(from, "GX_CopyTex A"));
    ASSERT_EQ(0u, log_count(from, "GX_LoadTexObj PD"));
    ASSERT_EQ(0u, log_count(from, "GX_SetZTexture"));
    ASSERT_EQ(1u, log_count(from, "GX_SetZMode 0 7 0"));
    ASSERT_EQ(1u, log_count(from, "GX_SetNumTevStages 1"));
    // one that writes depth: the depth parked before it, once, and put back through the Z texture
    wl_gx_target_drawn();
    (void)wl_gx_target_use(&b);
    from = g_gx_stub_log_count;
    wl_gx_target_depth_write();
    wl_gx_target_clear(false, true, 0, 1.0f, 0, 0, 32, 32);
    ASSERT_EQ(1u, log_count(from, "GX_CopyTex"));
    ASSERT_EQ(1u, log_count(from, "GX_CopyTex PD"));
    from = g_gx_stub_log_count;
    (void)wl_gx_target_use(NULL);
    ASSERT_EQ(1u, log_count(from, "GX_LoadTexObj PD 1"));
    ASSERT_EQ(1u, log_count(from, "GX_SetZTexture 2"));
    // (the back buffer: nothing to park)
    from = g_gx_stub_log_count;
    wl_gx_target_depth_write();
    ASSERT_EQ(from, g_gx_stub_log_count);
    wl_gx_target_stats(&stats, true);
    ASSERT_EQ(2u, stats.parks);
    ASSERT_EQ(1u, stats.depths);
    wl_gx_target_set_lazy_depth(false);
    PASS();
}

TEST squeeze_the_finished_picture_into_the_middle(void) {
    start();
    unsigned from = g_gx_stub_log_count;
    ASSERT(wl_gx_target_squeeze(80, 480));
    // the EFB out unfiltered into the colour block, cleared to black by the same copy (colour, alpha and depth
    // updates on first); the display copy's filter back; the block drawn as a linearly filtered quad over
    // columns 80 to 560 only: one stage, no Z texture
    EXPECT(from, "GX_SetZMode 1 7 1", "GX_SetColorUpdate 1", "GX_SetAlphaUpdate 1", "GX_SetCopyClear 0 0 0 255 0xFFFFFF",
           "copy_filter off",
           "wl_gpu_buffer_invalidate PC 1228800", "GX_SetTexCopySrc 0 0 640 480", "GX_SetTexCopyDst 640 480 0x6 0",
           "GX_CopyTex PC 1", "GX_PixModeSync", "copy_filter display", "GX_InvalidateTexAll",
           "GX_InitTexObj PC 640 480 0x6 0 0 0", "GX_InitTexObjFilterMode 1 1", "GX_LoadTexObj PC 0",
           "GX_SetScissor 0 0 640 480", "GX_SetZMode 0 7 0",
           "GX_LoadProjectionMtx ortho 0.003125 -1 -0.00416667 1 1 0 1", "GX_SetViewport 0 0 640 480 0 1",
           "GX_SetBlendMode 0 1 0 0", "GX_SetAlphaCompare 7 0 0 7 0", "GX_SetCullMode 0",
           "GX_SetNumIndStages 0", "GX_SetTevDirect 0", "GX_SetTevDirect 1", "GX_SetNumChans 0",
           "GX_SetNumTevStages 1", "GX_SetNumTexGens 1",
           "GX_SetTevOrder 0 0 0 255", "GX_SetTevSwapMode 0 0 0",
           "GX_SetTevColorIn 0 15 15 15 8", "GX_SetTevAlphaIn 0 7 7 7 4",
           "GX_SetTevColorOp 0 0 0 0 1 0", "GX_SetTevAlphaOp 0 0 0 0 1 0",
           "GX_LoadPosMtxImm identity 0", "GX_SetCurrentMtx 0", "GX_ClearVtxDesc",
           "GX_SetVtxDesc 9 1", "GX_SetVtxAttrFmt 0 9 1 4 0",
           "GX_SetVtxDesc 13 1", "GX_SetVtxAttrFmt 0 13 1 4 0", "GX_SetTexCoordGen 0 1 4 60",
           "GX_Begin 0x80 0 4",
           "GX_Position3f32 80 0 -1", "GX_TexCoord2f32 0 0",
           "GX_Position3f32 560 0 -1", "GX_TexCoord2f32 1 0",
           "GX_Position3f32 560 480 -1", "GX_TexCoord2f32 1 1",
           "GX_Position3f32 80 480 -1", "GX_TexCoord2f32 0 1",
           "GX_End", "wl_gx_invalidate_shadow");
    PASS();
}

TEST clear_quad_colour_and_depth(void) {
    start();
    unsigned from = g_gx_stub_log_count;
    wl_gx_target_clear(true, true, 0xFF804020u, 0.5f, 10, 20, 100, 50);
    EXPECT(from, "GX_SetColorUpdate 1", "GX_SetAlphaUpdate 1", "GX_SetZMode 1 7 1", "GX_SetZCompLoc 1",
           "GX_LoadProjectionMtx ortho 0.003125 -1 -0.00416667 1 1 0 1", "GX_SetViewport 0 0 640 480 0 1",
           "GX_SetBlendMode 0 1 0 0", "GX_SetAlphaCompare 7 0 0 7 0", "GX_SetCullMode 0",
           "GX_SetNumIndStages 0", "GX_SetTevDirect 0", "GX_SetTevDirect 1", "GX_SetNumChans 1",
           "GX_SetChanCtrl 4 0 0 0 0 0 2", "GX_SetNumTevStages 1", "GX_SetNumTexGens 0",
           // the colour in register 0 (red, green, blue, alpha), passed through as the stage's output
           "GX_SetTevColor 1 255 128 64 32",
           "GX_SetTevOrder 0 255 255 255", "GX_SetTevSwapMode 0 0 0",
           "GX_SetTevColorIn 0 15 15 15 2", "GX_SetTevAlphaIn 0 7 7 7 1",
           "GX_SetTevColorOp 0 0 0 0 1 0", "GX_SetTevAlphaOp 0 0 0 0 1 0",
           "GX_LoadPosMtxImm identity 0", "GX_SetCurrentMtx 0", "GX_ClearVtxDesc",
           "GX_SetVtxDesc 9 1", "GX_SetVtxAttrFmt 0 9 1 4 0",
           "GX_Begin 0x80 0 4",
           "GX_Position3f32 10 20 -0.5", "GX_Position3f32 110 20 -0.5", "GX_Position3f32 110 70 -0.5",
           "GX_Position3f32 10 70 -0.5", "GX_End", "wl_gx_invalidate_shadow");
    PASS();
}

TEST clear_quad_depth_only_and_colour_only(void) {
    start();
    unsigned from = g_gx_stub_log_count;
    wl_gx_target_clear(false, true, 0, 1.0f, 0, 0, 640, 480);
    ASSERT_STR_EQ("GX_SetColorUpdate 0", g_gx_stub_log[from]);
    ASSERT_STR_EQ("GX_SetAlphaUpdate 0", g_gx_stub_log[from + 1]);
    ASSERT_STR_EQ("GX_SetZMode 1 7 1", g_gx_stub_log[from + 2]);
    from = g_gx_stub_log_count;
    wl_gx_target_clear(true, false, 0, 0.0f, 0, 0, 640, 480);
    ASSERT_STR_EQ("GX_SetColorUpdate 1", g_gx_stub_log[from]);
    ASSERT_STR_EQ("GX_SetAlphaUpdate 1", g_gx_stub_log[from + 1]);
    ASSERT_STR_EQ("GX_SetZMode 0 7 0", g_gx_stub_log[from + 2]);
    ASSERT_STR_EQ("GX_SetZCompLoc 1", g_gx_stub_log[from + 3]);
    PASS();
}

TEST copies_leave_the_display_filter_on_and_note_the_commands(void) {
    start();
    WlGxTarget a = target_a();
    (void)wl_gx_target_use(&a);
    unsigned before = g_commands;
    (void)wl_gx_target_use(NULL);
    ASSERT(g_commands > before);
    // the last copy filter call is the display's
    unsigned last = 0;
    for (unsigned i = 0; i < g_gx_stub_log_count; i++) {
        if (strncmp(g_gx_stub_log[i], "copy_filter", 11) == 0) {
            last = i;
        }
    }
    ASSERT_STR_EQ("copy_filter display", g_gx_stub_log[last]);
    PASS();
}

// What the GPU alone writes is invalidated once: a block a copy has filled, and memory a target says it
// has been resolved into, is not invalidated by the next copy (the processor holds none of its lines).
static unsigned invalidations_since(unsigned from) {
    unsigned n = 0;
    for (unsigned i = from; i < g_gx_stub_log_count; i++) {
        n += strncmp(g_gx_stub_log[i], "wl_gpu_buffer_invalidate", 24) == 0 ? 1u : 0u;
    }
    return n;
}

TEST gpu_owned_blocks_are_invalidated_once(void) {
    start();
    WlGxTarget a = target_a();
    unsigned from = g_gx_stub_log_count;
    wl_gx_target_drawn();
    (void)wl_gx_target_use(&a); // parks: the colour and depth blocks, new
    ASSERT_EQ(2u, invalidations_since(from));
    from = g_gx_stub_log_count;
    (void)wl_gx_target_use(NULL); // a resolved (its memory is the caller's: invalidated)
    ASSERT_EQ(1u, invalidations_since(from));
    from = g_gx_stub_log_count;
    wl_gx_target_frame_begun();
    wl_gx_target_drawn();
    (void)wl_gx_target_use(&a); // parks again, into the same blocks
    ASSERT_EQ(0u, invalidations_since(from));
    // a's descriptor says its memory is the GPU's now (the caller learned it from the first resolve)
    a.resident = true;
    (void)wl_gx_target_use(&a);
    from = g_gx_stub_log_count;
    (void)wl_gx_target_use(NULL); // resolved again
    ASSERT_EQ(0u, invalidations_since(from));
    // the squeeze goes into the colour block, larger than the park's: a new block, invalidated once
    from = g_gx_stub_log_count;
    ASSERT(wl_gx_target_squeeze(80, 480));
    ASSERT_EQ(1u, invalidations_since(from));
    from = g_gx_stub_log_count;
    ASSERT(wl_gx_target_squeeze(80, 480));
    ASSERT_EQ(0u, invalidations_since(from));
    PASS();
}

SUITE(target_gx_suite) {
    RUN_TEST(gpu_owned_blocks_are_invalidated_once);
    RUN_TEST(park_the_back_buffer_and_take_the_efb);
    RUN_TEST(resolve_a_target_into_its_memory);
    RUN_TEST(put_the_back_buffer_back_through_a_z_texture);
    RUN_TEST(lazy_depth_parks_the_depth_only_before_a_target_writes_it);
    RUN_TEST(squeeze_the_finished_picture_into_the_middle);
    RUN_TEST(clear_quad_colour_and_depth);
    RUN_TEST(clear_quad_depth_only_and_colour_only);
    RUN_TEST(copies_leave_the_display_filter_on_and_note_the_commands);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(target_gx_suite);
    GREATEST_MAIN_END();
}
