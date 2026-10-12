// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_gx: the NV2A backend over GX, run on the host's recorder of wl/platform's direct GX
// layer (host/gx_direct_host.h). Synthetic push buffers: a quad in screen space with a texture and a
// combiner program (the vertices as GX gets them, the TEV program applied, the texture bound), a
// draw through a program that divides by w (the perspective projection and (x w, y w, -w)), the
// frame's clear, culling, a stencil draw skipped, and the frame shown when the title finishes it.
#include <math.h>
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "wl/platform/gx_target.h"
#include "wl/platform/host/gx_direct_host.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/host/tev_host.h"
#include "wl/xbox/nv2a_gx.h"

// (assembled with tests/xbox/vsh_programs.py's assembler)
static const uint32_t screen_program[][4] = {
    {0x00000000, 0x0020001B, 0x0836006C, 0x0000F800}, // mov oPos, v0
    {0x00000000, 0x0020061B, 0x0836006C, 0x0000F818}, // mov oD0, v3
    {0x00000000, 0x0020121B, 0x0836006C, 0x0000F849}, // mov oT0, v9
};
static const uint32_t clip_program[][4] = {
    {0x00000000, 0x00EC001B, 0x0836186C, 0x08C00000}, // dp4 r12.x, v0, c[0]
    {0x00000000, 0x00EC201B, 0x0836186C, 0x04C00000}, // dp4 r12.y, v0, c[1]
    {0x00000000, 0x00EC401B, 0x0836186C, 0x02C00000}, // dp4 r12.z, v0, c[2]
    {0x00000000, 0x00EC601B, 0x0836186C, 0x01C00000}, // dp4 r12.w, v0, c[3]
    {0x00000000, 0x0600001B, 0x003603FF, 0x10180000}, // ircc r1.x, r12.w
    {0x00000000, 0x0040001B, 0xC400286C, 0x0EC00000}, // mul r12.xyz, r12, r1.x
    {0x00000000, 0x0027601B, 0x0C36006C, 0x0F200000}, // mov r2, c[-37]
    {0x00000000, 0x0087401B, 0xC436186C, 0x9000E800}, // mad oPos.xyz, r12, c[-38], r2
    {0x00000000, 0x0020061B, 0x0836006C, 0x0000F818}, // mov oD0, v3
    {0x00000000, 0x0020121B, 0x0836006C, 0x0000F849}, // mov oT0, v9
};

static void load_program(TGuest *g, uint32_t slot, const uint32_t (*program)[4], unsigned count) {
    unsigned i, w;
    t_m1(g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_LOAD, slot);
    for (i = 0; i < count; i++) {
        t_method(g, WL_NV2A_K_SET_TRANSFORM_PROGRAM, 4);
        for (w = 0; w < 4; w++) {
            t_word(g, program[i][w]);
        }
    }
    t_m1(g, WL_NV2A_K_SET_TRANSFORM_PROGRAM_START, slot);
    t_m1(g, WL_NV2A_K_SET_TRANSFORM_EXECUTION_MODE, WL_NV2A_XF_PROGRAM);
}

static void constant4(TGuest *g, uint32_t index, float x, float y, float z, float w) {
    t_m1(g, WL_NV2A_K_SET_TRANSFORM_CONSTANT_LOAD, index);
    t_method(g, WL_NV2A_K_SET_TRANSFORM_CONSTANT, 4);
    t_word(g, t_f(x));
    t_word(g, t_f(y));
    t_word(g, t_f(z));
    t_word(g, t_f(w));
}

// the back buffer: 640x480 A8R8G8B8 with a Z24S8 depth; one combiner stage, r0 = t0 * v0, and the
// final combiner's output r0
static void frame_setup(TGuest *g) {
    t_m1(g, WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL, 640u << 16);
    t_m1(g, WL_NV2A_K_SET_SURFACE_CLIP_VERTICAL, 480u << 16);
    t_m1(g, WL_NV2A_K_SET_SURFACE_FORMAT,
         WL_NV2A_SURFACE_COLOR_A8R8G8B8 | WL_NV2A_SURFACE_ZETA_Z24S8 << 4 | WL_NV2A_SURFACE_TYPE_PITCH << 8);
    t_m1(g, WL_NV2A_K_SET_SURFACE_PITCH, 2560u << 16 | 2560u);
    t_m1(g, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET, 0x100000);
    t_m1(g, WL_NV2A_K_SET_SURFACE_ZETA_OFFSET, 0x200000);
    t_m1(g, WL_NV2A_K_SET_COMBINER_COLOR_ICW, 0x08040000u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_COLOR_OCW, 0x000000C0u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_ALPHA_ICW, 0x18140000u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_ALPHA_OCW, 0x000000C0u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW0, 0x0000000Cu);
    t_m1(g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001C00u);
    t_m1(g, WL_NV2A_K_SET_COMBINER_CONTROL, 1);
    t_m1(g, WL_NV2A_K_SET_SHADER_STAGE_PROGRAM, WL_NV2A_STAGE_2D_PROJECTIVE);
    // a 4x4 A8R8G8B8 texture at 0x60000
    t_m1(g, WL_NV2A_K_SET_TEXTURE_OFFSET, 0x60000);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_FORMAT,
         1u | 2u << 4 | (uint32_t)WL_NV2A_TEX_SZ_A8R8G8B8 << 8 | 1u << 16 | 2u << 20 | 2u << 24);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_CONTROL0, 1u << 30);
    t_m1(g, WL_NV2A_K_SET_TEXTURE_FILTER, 2u << 16 | 2u << 24);
    t_m1(g, WL_NV2A_K_SET_COLOR_MASK, 0x01010101u);
    t_m1(g, WL_NV2A_K_SET_DEPTH_TEST_ENABLE, 1);
    t_m1(g, WL_NV2A_K_SET_DEPTH_FUNC, WL_NV2A_FUNC_LEQUAL);
    t_m1(g, WL_NV2A_K_SET_DEPTH_MASK, 1);
}

// an immediate vertex: texture coordinate (2F), colour (4UB), position (4F, completes)
static void immediate_vertex(TGuest *g, float x, float y, float z, float w, float s, float t, uint32_t abgr) {
    t_method(g, WL_NV2A_K_SET_VERTEX_DATA2F_M + 8 * 9, 2);
    t_word(g, t_f(s));
    t_word(g, t_f(t));
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA4UB + 4 * 3, abgr);
    t_method(g, WL_NV2A_K_SET_VERTEX_DATA4F_M, 4);
    t_word(g, t_f(x));
    t_word(g, t_f(y));
    t_word(g, t_f(z));
    t_word(g, t_f(w));
}

static WlNv2aGx *g_gx;

static WlNv2a *gx_device(TGuest *g) {
    WlNv2aBackend backend;
    unsigned i;

    wl_gx_init();
    wl_host_gx_direct_reset();
    wl_host_tev_reset();
    wl_host_gx_clear_counters();
    g_gx = wl_nv2a_gx_create(NULL);
    if (g_gx == NULL) {
        return NULL;
    }
    wl_nv2a_gx_backend(g_gx, &backend);
    if (t_create(g, &backend) == NULL) {
        return NULL;
    }
    wl_nv2a_gx_attach(g_gx, g->dev);
    for (i = 0; i < 16; i++) {
        t_w32(g, 0x60000 + 4 * i, 0xFF808080u); // the texture: grey
    }
    t_setup_3d(g);
    return g->dev;
}

static void gx_done(TGuest *g) {
    wl_nv2a_gx_destroy(g_gx);
    g_gx = NULL;
    t_destroy(g);
    wl_gx_shutdown();
}

static int near(float a, float b) {
    return fabsf(a - b) < 1e-3f;
}

TEST screen_space_quad_with_texture_and_combiners(void) {
    TGuest g;
    const WlHostGxDirect *d;
    WlNv2aGxStats stats;
    ASSERT(gx_device(&g) != NULL);
    frame_setup(&g);
    load_program(&g, 0, screen_program, 3);
    // a clear of everything (the frame's start: GX's own clear), then the quad
    t_m1(&g, WL_NV2A_K_SET_COLOR_CLEAR_VALUE, 0xFF204080u);
    t_m1(&g, WL_NV2A_K_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00u);
    t_m1(&g, WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL, 639u << 16);
    t_m1(&g, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL, 479u << 16);
    t_m1(&g, WL_NV2A_K_CLEAR_SURFACE, WL_NV2A_CLEAR_COLOR | WL_NV2A_CLEAR_Z | WL_NV2A_CLEAR_STENCIL);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_QUADS);
    immediate_vertex(&g, 100, 100, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 200, 100, 0, 1, 1, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 200, 200, 0, 1, 1, 1, 0xFFFFFFFFu);
    immediate_vertex(&g, 100, 200, 0, 1, 0, 1, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);

    d = wl_host_gx_direct();
    ASSERT_EQ(WL_GX_DIRECT_QUADS, d->prim);
    ASSERT_EQ(4u, d->count);
    // pixel centres on integers, as Direct3D puts them; depth 0 sent as z - 1
    ASSERT(near(d->vertices[0].position[0], 100.5f));
    ASSERT(near(d->vertices[2].position[1], 200.5f));
    ASSERT(near(d->vertices[0].position[2], -1.0f));
    ASSERT_EQ(255, d->vertices[1].color[0][0]);
    ASSERT(near(d->vertices[2].texcoord[0][0], 1.0f));
    ASSERT(near(d->vertices[3].texcoord[0][1], 1.0f));
    ASSERT(d->orthographic);
    ASSERT(near(d->projection[0][0], 2.0f / 640.0f));
    // the combiners' program on the TEV, reading texture 0 and colour 0; the texture on map 0
    ASSERT(wl_host_tev_last() != NULL);
    ASSERT(wl_host_tev_last()->stage_count >= 1);
    ASSERT(d->textures[0] != NULL);
    ASSERT_EQ(4u, d->textures[0]->width);
    ASSERT(d->layout.texcoords & 1u);
    ASSERT(d->layout.colors >= 1);
    ASSERT(d->z_test);
    ASSERT_EQ(WL_GX_LEQUAL, d->z_func);
    // the frame's first full clear was GX's own (no quad), with its colour
    ASSERT_EQ(0x204080FFu, wl_host_gx_state()->clear_rgba);
    wl_nv2a_gx_stats(g_gx, &stats);
    ASSERT_EQ(1u, stats.draws);
    ASSERT_EQ(0u, stats.quad_clears);
    ASSERT_EQ(0u, stats.programs_unusable);
    // the title finishes the frame: shown
    ASSERT_EQ(0u, wl_host_gx_state()->end_frames);
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(&g);
    ASSERT_EQ(1u, wl_host_gx_state()->end_frames);
    gx_done(&g);
    PASS();
}

TEST perspective_draws_send_x_w_y_w_minus_w(void) {
    TGuest g;
    const WlHostGxDirect *d;
    ASSERT(gx_device(&g) != NULL);
    frame_setup(&g);
    load_program(&g, 8, clip_program, 10);
    // clip = (x, y, z / 2, z): w the depth along z
    constant4(&g, 96, 1, 0, 0, 0);
    constant4(&g, 97, 0, 1, 0, 0);
    constant4(&g, 98, 0, 0, 0.5f, 0);
    constant4(&g, 99, 0, 0, 1, 0);
    t_method(&g, WL_NV2A_K_SET_VIEWPORT_SCALE, 4);
    t_word(&g, t_f(320.0f));
    t_word(&g, t_f(-240.0f));
    t_word(&g, t_f(16777215.0f));
    t_word(&g, t_f(0.0f));
    t_method(&g, WL_NV2A_K_SET_VIEWPORT_OFFSET, 4);
    t_word(&g, t_f(320.0f));
    t_word(&g, t_f(240.0f));
    t_word(&g, t_f(0.0f));
    t_word(&g, t_f(0.0f));
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    immediate_vertex(&g, 0, 0, 2, 1, 0, 0, 0xFF0000FFu);
    immediate_vertex(&g, 1, 1, 4, 1, 1, 0, 0xFF0000FFu);
    immediate_vertex(&g, -1, 1, 4, 1, 0, 1, 0xFF0000FFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    d = wl_host_gx_direct();
    ASSERT_EQ(WL_GX_DIRECT_TRIANGLES, d->prim);
    ASSERT_FALSE(d->orthographic);
    // (0, 0, 2): clip (0, 0, 1, 2), the screen's middle; sent as pixels times w, -w
    ASSERT(near(d->vertices[0].position[2], -2.0f));
    ASSERT(near(d->vertices[0].position[0] / 2.0f, 320.5f));
    ASSERT(near(d->vertices[0].position[1] / 2.0f, 240.5f));
    // (1, 1, 4): clip (1, 1, 2, 4), at (400, 180)
    ASSERT(near(d->vertices[1].position[0] / 4.0f, 400.5f));
    ASSERT(near(d->vertices[1].position[1] / 4.0f, 180.5f));
    ASSERT_EQ(255, d->vertices[1].color[0][0]);
    // the projection maps (X, Y, -w) back to the screen: x' = 2/W X + Z
    ASSERT(near(d->projection[0][0], 2.0f / 640.0f));
    ASSERT(near(d->projection[0][2], 1.0f));
    ASSERT(near(d->projection[3][2], -1.0f));
    {
        WlNv2aGxStats stats;
        wl_nv2a_gx_stats(g_gx, &stats);
        ASSERT_EQ(1u, stats.perspective_draws);
    }
    gx_done(&g);
    PASS();
}

TEST culling_by_the_nv2a_rule_and_stencil_skipped(void) {
    TGuest g;
    const WlHostGxDirect *d;
    WlNv2aGxStats stats;
    ASSERT(gx_device(&g) != NULL);
    frame_setup(&g);
    load_program(&g, 0, screen_program, 3);
    // front faces CW (negative area in the screen's y-down coordinates), back faces culled: the
    // positive-area ones (the host's measure reads 0: GX_CULL_FRONT culls positive area)
    t_m1(&g, WL_NV2A_K_SET_CULL_FACE_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_CULL_FACE, WL_NV2A_CULL_BACK);
    t_m1(&g, WL_NV2A_K_SET_FRONT_FACE, WL_NV2A_FRONT_CW);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    immediate_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    d = wl_host_gx_direct();
    ASSERT_EQ(WL_GX_CULL_FRONT, d->cull);
    t_m1(&g, WL_NV2A_K_SET_FRONT_FACE, WL_NV2A_FRONT_CCW);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    immediate_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT_EQ(WL_GX_CULL_BACK, d->cull);
    // a draw with the stencil test: skipped
    t_m1(&g, WL_NV2A_K_SET_STENCIL_TEST_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    immediate_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    wl_nv2a_gx_stats(g_gx, &stats);
    ASSERT_EQ(2u, stats.draws);
    ASSERT_EQ(1u, stats.stencil_skipped);
    gx_done(&g);
    PASS();
}

TEST partial_clears_are_quads_and_render_targets_take_the_efb(void) {
    TGuest g;
    WlNv2aGxStats stats;
    WlGxTargetStats targets;
    ASSERT(gx_device(&g) != NULL);
    frame_setup(&g);
    load_program(&g, 0, screen_program, 3);
    wl_gx_target_stats(&targets, true);
    // a clear of a part: a quad
    t_m1(&g, WL_NV2A_K_SET_COLOR_CLEAR_VALUE, 0xFF000000u);
    t_m1(&g, WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL, 99u << 16 | 10u);
    t_m1(&g, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL, 99u << 16 | 10u);
    t_m1(&g, WL_NV2A_K_CLEAR_SURFACE, WL_NV2A_CLEAR_COLOR);
    t_kick(&g);
    wl_nv2a_gx_stats(g_gx, &stats);
    ASSERT_EQ(1u, stats.quad_clears);
    // a 128x128 swizzled surface at 0x70000: a render target, its texture then read from the image
    t_m1(&g, WL_NV2A_K_SET_SURFACE_FORMAT,
         WL_NV2A_SURFACE_COLOR_A8R8G8B8 | WL_NV2A_SURFACE_TYPE_SWIZZLE << 8 | 7u << 16 | 7u << 24);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL, 128u << 16);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_CLIP_VERTICAL, 128u << 16);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET, 0x70000);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    immediate_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    // back to the screen, sampling the target's address
    t_m1(&g, WL_NV2A_K_SET_SURFACE_FORMAT,
         WL_NV2A_SURFACE_COLOR_A8R8G8B8 | WL_NV2A_SURFACE_ZETA_Z24S8 << 4 | WL_NV2A_SURFACE_TYPE_PITCH << 8);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_CLIP_HORIZONTAL, 640u << 16);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_CLIP_VERTICAL, 480u << 16);
    t_m1(&g, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET, 0x100000);
    t_m1(&g, WL_NV2A_K_SET_TEXTURE_OFFSET, 0x70000);
    t_m1(&g, WL_NV2A_K_SET_TEXTURE_FORMAT,
         1u | 2u << 4 | (uint32_t)WL_NV2A_TEX_SZ_A8R8G8B8 << 8 | 1u << 16 | 7u << 20 | 7u << 24);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    immediate_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    immediate_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    wl_nv2a_gx_stats(g_gx, &stats);
    ASSERT_EQ(1u, stats.target_draws);
    wl_gx_target_stats(&targets, false);
    ASSERT_EQ(1u, targets.resolves);
    // the texture at the target's address is the target's image (128x128 RGBA8), not converted (the
    // one converted is the grey texture the target's draw read)
    ASSERT(wl_host_gx_direct()->textures[0] != NULL);
    ASSERT_EQ(128u, wl_host_gx_direct()->textures[0]->width);
    {
        WlNv2aTexCacheStats ts;
        wl_nv2a_texcache_stats(wl_nv2a_gx_textures(g_gx), &ts);
        ASSERT_EQ(1u, ts.converted);
    }
    gx_done(&g);
    PASS();
}

SUITE(gx_suite) {
    RUN_TEST(screen_space_quad_with_texture_and_combiners);
    RUN_TEST(perspective_draws_send_x_w_y_w_minus_w);
    RUN_TEST(culling_by_the_nv2a_rule_and_stencil_skipped);
    RUN_TEST(partial_clears_are_quads_and_render_targets_take_the_efb);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gx_suite);
    GREATEST_MAIN_END();
}
