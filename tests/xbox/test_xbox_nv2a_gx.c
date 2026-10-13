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
#include "nv2a_frames.h"
#include "wl/platform/gx_target.h"
#include "wl/platform/host/gx_direct_host.h"
#include "wl/platform/host/gx_host.h"
#include "wl/platform/host/tev_host.h"
#include "wl/xbox/nv2a_gx.h"

static WlNv2aGx *g_gx;

static WlNv2a *gx_device(TGuest *g) {
    WlNv2aBackend backend;

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
    f_frame_setup(&g);
    f_program(&g, 0, f_screen_program, 3);
    // a clear of everything (the frame's start: GX's own clear), then the quad
    t_m1(&g, WL_NV2A_K_SET_COLOR_CLEAR_VALUE, 0xFF204080u);
    t_m1(&g, WL_NV2A_K_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00u);
    t_m1(&g, WL_NV2A_K_SET_CLEAR_RECT_HORIZONTAL, 639u << 16);
    t_m1(&g, WL_NV2A_K_SET_CLEAR_RECT_VERTICAL, 479u << 16);
    t_m1(&g, WL_NV2A_K_CLEAR_SURFACE, WL_NV2A_CLEAR_COLOR | WL_NV2A_CLEAR_Z | WL_NV2A_CLEAR_STENCIL);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_QUADS);
    f_vertex(&g, 100, 100, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 200, 100, 0, 1, 1, 0, 0xFFFFFFFFu);
    f_vertex(&g, 200, 200, 0, 1, 1, 1, 0xFFFFFFFFu);
    f_vertex(&g, 100, 200, 0, 1, 0, 1, 0xFFFFFFFFu);
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
    f_frame_setup(&g);
    f_perspective(&g);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    f_vertex(&g, 0, 0, 2, 1, 0, 0, 0xFF0000FFu);
    f_vertex(&g, 1, 1, 4, 1, 1, 0, 0xFF0000FFu);
    f_vertex(&g, -1, 1, 4, 1, 0, 1, 0xFF0000FFu);
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
    f_frame_setup(&g);
    f_program(&g, 0, f_screen_program, 3);
    // front faces CW (clockwise as the screen shows it: positive area in its y-down coordinates,
    // Direct3D's D3DCULL_CCW), back faces culled: the negative-area ones (the host's measure reads
    // 0: GX_CULL_FRONT culls positive area, so GX_CULL_BACK the negative)
    t_m1(&g, WL_NV2A_K_SET_CULL_FACE_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_CULL_FACE, WL_NV2A_CULL_BACK);
    t_m1(&g, WL_NV2A_K_SET_FRONT_FACE, WL_NV2A_FRONT_CW);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    f_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    d = wl_host_gx_direct();
    ASSERT_EQ(WL_GX_CULL_BACK, d->cull);
    t_m1(&g, WL_NV2A_K_SET_FRONT_FACE, WL_NV2A_FRONT_CCW);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    f_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT_EQ(WL_GX_CULL_FRONT, d->cull);
    // a draw with the stencil test: skipped
    t_m1(&g, WL_NV2A_K_SET_STENCIL_TEST_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    f_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
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
    f_frame_setup(&g);
    f_program(&g, 0, f_screen_program, 3);
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
    f_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
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
    f_vertex(&g, 0, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 10, 0, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 0, 10, 0, 1, 0, 0, 0xFFFFFFFFu);
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
