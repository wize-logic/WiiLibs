// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_raster: the software reference of the GX backend, from push buffer to pixel. The
// frame's clear; a textured quad through the combiners' TEV program (texture times colour); depth
// tested quads; a two-stage combiner program with per-stage constants; alpha blending; a triangle
// through a program that divides by w; a frame handed over when the title finishes it.
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "nv2a_frames.h"
#include "wl/xbox/nv2a_raster.h"

static WlNv2aRaster *g_raster;
static unsigned g_frames_seen;
static uint32_t g_frame_phys;

static void on_frame(void *user, uint32_t phys, const uint8_t *rgba, uint32_t w, uint32_t h) {
    (void)user;
    (void)rgba;
    (void)w;
    (void)h;
    g_frames_seen++;
    g_frame_phys = phys;
}

static WlNv2a *raster_device(TGuest *g) {
    WlNv2aRasterConfig cfg;
    WlNv2aBackend backend;

    memset(&cfg, 0, sizeof cfg);
    cfg.frame = on_frame;
    g_frames_seen = 0;
    g_raster = wl_nv2a_raster_create(&cfg);
    if (g_raster == NULL) {
        return NULL;
    }
    wl_nv2a_raster_backend(g_raster, &backend);
    if (t_create(g, &backend) == NULL) {
        return NULL;
    }
    wl_nv2a_raster_attach(g_raster, g->dev);
    t_setup_3d(g);
    f_frame_setup(g);
    return g->dev;
}

static void raster_done(TGuest *g) {
    wl_nv2a_raster_destroy(g_raster);
    g_raster = NULL;
    t_destroy(g);
}

// the pixel's RGBA as 0xRRGGBBAA
static uint32_t pixel(uint32_t x, uint32_t y) {
    uint32_t w, h;
    const uint8_t *p = wl_nv2a_raster_surface(g_raster, F_COLOR, &w, &h);

    if (p == NULL || x >= w || y >= h) {
        return 0xDEADBEEFu;
    }
    p += ((size_t)y * w + x) * 4u;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int near_rgb(uint32_t got, uint32_t want, int slack) {
    int shift;
    for (shift = 8; shift < 32; shift += 8) {
        int d = (int)((got >> shift) & 0xFFu) - (int)((want >> shift) & 0xFFu);
        if (d > slack || d < -slack) {
            return 0;
        }
    }
    return 1;
}

TEST textured_quad_through_the_combiners(void) {
    TGuest g;
    ASSERT(raster_device(&g) != NULL);
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF204080u);
    // a white quad over (100, 100)-(200, 200): the grey texture times white
    f_quad(&g, 100, 100, 200, 200, 0.5f, 0xFFFFFFFFu);
    t_kick(&g);
    ASSERT(near_rgb(pixel(50, 50), 0x204080FFu, 0));
    ASSERT(near_rgb(pixel(150, 150), 0x808080FFu, 1));
    // the edges: pixel centres on integers, a pixel's centre at +0.5 (the quad covers 100..199)
    ASSERT(near_rgb(pixel(100, 100), 0x808080FFu, 1));
    ASSERT(near_rgb(pixel(199, 199), 0x808080FFu, 1));
    ASSERT(near_rgb(pixel(200, 150), 0x204080FFu, 0));
    ASSERT(near_rgb(pixel(99, 150), 0x204080FFu, 0));
    // a colour: red times the grey
    f_quad(&g, 300, 100, 400, 200, 0.5f, 0xFF0000FFu);
    t_kick(&g);
    ASSERT(near_rgb(pixel(350, 150), 0x800000FFu, 1));
    // the frame finished: handed over
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(&g);
    ASSERT_EQ(1u, g_frames_seen);
    ASSERT_EQ((uint32_t)F_COLOR, g_frame_phys);
    raster_done(&g);
    PASS();
}

TEST depth_test_keeps_the_nearer(void) {
    TGuest g;
    ASSERT(raster_device(&g) != NULL);
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF000000u);
    // red at depth 0.3, then green over it at 0.7 (LEQUAL: behind, not drawn), blue at 0.1 (drawn)
    f_quad(&g, 100, 100, 200, 200, 0.3f, 0xFF0000FFu);
    f_quad(&g, 150, 150, 250, 250, 0.7f, 0xFF00FF00u);
    f_quad(&g, 180, 180, 220, 220, 0.1f, 0xFFFF0000u);
    t_kick(&g);
    ASSERT(near_rgb(pixel(160, 160), 0x800000FFu, 1)); // red stays where green is behind it
    ASSERT(near_rgb(pixel(240, 240), 0x008000FFu, 1)); // green where nothing was
    ASSERT(near_rgb(pixel(190, 190), 0x000080FFu, 1)); // blue in front
    raster_done(&g);
    PASS();
}

TEST two_combiner_stages_with_their_constants(void) {
    TGuest g;
    unsigned i;
    ASSERT(raster_device(&g) != NULL);
    for (i = 0; i < 16; i++) {
        t_w32(&g, F_TEXTURE + 4 * i, 0xFFFFFFFFu); // white
    }
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF000000u);
    // r1 = t0 * c0 + v0 * c1: white * (255, 128, 0) + grey vertex (128) * (0, 0, 128)
    f_combiner_two_stages(&g, 0xFFFF8000u, 0xFF000080u);
    f_quad(&g, 0, 0, 64, 64, 0.5f, 0xFF808080u);
    t_kick(&g);
    ASSERT(near_rgb(pixel(32, 32), 0xFF8040FFu, 2));
    raster_done(&g);
    PASS();
}

TEST alpha_blending(void) {
    TGuest g;
    ASSERT(raster_device(&g) != NULL);
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF000000u);
    {
        unsigned i;
        for (i = 0; i < 16; i++) {
            t_w32(&g, F_TEXTURE + 4 * i, 0xFFFFFFFFu);
        }
    }
    f_quad(&g, 0, 0, 64, 64, 0.5f, 0xFF0000FFu); // red
    // half-transparent blue over it: src alpha, 1 - src alpha
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_SFACTOR, WL_NV2A_BLEND_SRC_ALPHA);
    t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR, WL_NV2A_BLEND_ONE_MINUS_SRC_ALPHA);
    t_m1(&g, WL_NV2A_K_SET_BLEND_EQUATION, WL_NV2A_BLEND_EQ_ADD);
    t_m1(&g, WL_NV2A_K_SET_DEPTH_TEST_ENABLE, 0);
    f_quad(&g, 0, 0, 64, 64, 0.5f, 0x80FF0000u);
    t_kick(&g);
    ASSERT(near_rgb(pixel(32, 32), 0x7F0080FFu, 2));
    raster_done(&g);
    PASS();
}

TEST perspective_triangle(void) {
    TGuest g;
    ASSERT(raster_device(&g) != NULL);
    f_perspective(&g);
    f_clear(&g, 0xFF000000u);
    {
        unsigned i;
        for (i = 0; i < 16; i++) {
            t_w32(&g, F_TEXTURE + 4 * i, 0xFFFFFFFFu);
        }
    }
    // a triangle at w 2..4 around the screen's middle: (0,0,2) the middle, (1,1,4) at (400,180),
    // (-1,1,4) at (240,180)
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    f_vertex(&g, 0, 0, 2, 1, 0, 0, 0xFF00FF00u);
    f_vertex(&g, 1, 1, 4, 1, 1, 0, 0xFF00FF00u);
    f_vertex(&g, -1, 1, 4, 1, 0, 1, 0xFF00FF00u);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT(near_rgb(pixel(320, 200), 0x00FF00FFu, 1));
    ASSERT(near_rgb(pixel(320, 260), 0x000000FFu, 0));
    ASSERT(near_rgb(pixel(420, 200), 0x000000FFu, 0));
    raster_done(&g);
    PASS();
}

SUITE(raster_suite) {
    RUN_TEST(textured_quad_through_the_combiners);
    RUN_TEST(depth_test_keeps_the_nearer);
    RUN_TEST(two_combiner_stages_with_their_constants);
    RUN_TEST(alpha_blending);
    RUN_TEST(perspective_triangle);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(raster_suite);
    GREATEST_MAIN_END();
}
