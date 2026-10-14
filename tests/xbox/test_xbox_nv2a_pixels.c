// SPDX-License-Identifier: MIT
// The NV2A backend on GX (wl/xbox/nv2a_gx.h) drawn by the host's software GX (wl/gxsoft): from push
// buffer to the pixels GX would show. First the console suite (tests/xbox/suite_nv2a.c, whose
// expectations are the EFB pixels Dolphin produced) run whole on the host; then the frame's clear,
// a textured quad through the combiners' TEV program, the depth test, two combiner stages with
// their constants, alpha blending, a triangle through a program that divides by w, Direct3D's
// culling of screen-space quads, a program whose final product needs every TEV register, and a
// capture replayed to the same pixels (written out for the replay tool's test). Draws from vertex
// arrays through GX's transform unit against the same draws on the processor, within a tolerance,
// and a second pass over them by a program whose colour the processor computes (the transform unit
// still doing its position) passing LEQUAL everywhere; the same draw with its arrays kept for GX to
// read by index (the vertex cache), to the bit, and again after its vertices are rewritten.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "nv2a_test_util.h"
#include "nv2a_frames.h"
#include "tests/platform/check.h"
#include "wl/gxsoft/gxsoft.h"
#include "wl/platform/platform.h"
#include "wl/xbox/nv2a_capture.h"
#include "wl/xbox/nv2a_gx.h"

void wlt_suite_nv2a(void);

static unsigned g_checks, g_failures;

void wlt_check(const char *name, bool ok, const char *detail) {
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL %s: %s\n", name, detail != NULL ? detail : "");
    }
}

void wlt_report(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    printf("  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

// ---- a device over the GX backend over the software GX ---------------------------------------------

static WlNv2aGx *g_gx;
static uint8_t *g_frame;
static uint32_t g_frame_w, g_frame_h;
static unsigned g_frames_seen;
static WlGxSoftDraw g_last_draw;

static void on_frame(void *user, const uint8_t *rgba, uint32_t w, uint32_t h) {
    (void)user;
    free(g_frame);
    g_frame = malloc((size_t)w * h * 4u);
    if (g_frame != NULL) {
        memcpy(g_frame, rgba, (size_t)w * h * 4u);
    }
    g_frame_w = w;
    g_frame_h = h;
    g_frames_seen++;
}

static void on_draw(void *user, const WlGxSoftDraw *d) {
    (void)user;
    g_last_draw = *d;
}

// (the next pixel_device's GX backend never uses the transform unit)
static bool g_no_xf;
static size_t g_vertex_cache;

static WlNv2a *pixel_device(TGuest *g) {
    WlGxSoftConfig cfg;
    WlNv2aBackend backend;

    memset(&cfg, 0, sizeof cfg);
    cfg.frame = on_frame;
    cfg.draw = on_draw;
    g_frames_seen = 0;
    if (wl_gx_init() != WL_OK || wl_gxsoft_attach(&cfg) != WL_OK) {
        return NULL;
    }
    {
        WlNv2aGxConfig gcfg;

        memset(&gcfg, 0, sizeof gcfg);
        gcfg.no_xf = g_no_xf;
        gcfg.vertices.budget = g_vertex_cache;
        g_gx = wl_nv2a_gx_create(&gcfg);
    }
    if (g_gx == NULL) {
        return NULL;
    }
    wl_nv2a_gx_backend(g_gx, &backend);
    if (t_create(g, &backend) == NULL) {
        return NULL;
    }
    wl_nv2a_gx_attach(g_gx, g->dev);
    t_setup_3d(g);
    f_frame_setup(g);
    return g->dev;
}

static void pixel_done(TGuest *g) {
    wl_nv2a_gx_destroy(g_gx);
    g_gx = NULL;
    t_destroy(g);
    wl_gxsoft_detach();
    wl_gx_shutdown();
    free(g_frame);
    g_frame = NULL;
}

// an EFB pixel as 0xRRGGBBAA (the frame still open)
static uint32_t pixel(int x, int y) {
    return wl_gx_peek_rgba(x, y);
}

// a pixel of the last frame shown
static uint32_t shown(uint32_t x, uint32_t y) {
    const uint8_t *p;

    if (g_frame == NULL || x >= g_frame_w || y >= g_frame_h) {
        return 0xDEADBEEFu;
    }
    p = g_frame + ((size_t)y * g_frame_w + x) * 4u;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// the colour channels within `slack` (the EFB keeps six bits a channel with a destination alpha)
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

static void white_texture(TGuest *g) {
    unsigned i;

    for (i = 0; i < 16; i++) {
        t_w32(g, F_TEXTURE + 4 * i, 0xFFFFFFFFu);
    }
}

// ---- tests ----------------------------------------------------------------------------------------

TEST console_suite_on_the_software_gx(void) {
    ASSERT_EQ(WL_OK, wl_gx_init());
    ASSERT_EQ(WL_OK, wl_gxsoft_attach(NULL));
    g_checks = g_failures = 0;
    wlt_suite_nv2a();
    wl_gxsoft_detach();
    wl_gx_shutdown();
    ASSERT(g_checks > 10u);
    ASSERT_EQ(0u, g_failures);
    PASS();
}

TEST textured_quad_through_the_combiners(void) {
    TGuest g;

    ASSERT(pixel_device(&g) != NULL);
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF204080u);
    // a white quad over (100, 100)-(200, 200): the grey texture times white
    f_quad(&g, 100, 100, 200, 200, 0.5f, 0xFFFFFFFFu);
    t_kick(&g);
    ASSERT(near_rgb(pixel(50, 50), 0x204080FFu, 4));
    ASSERT(near_rgb(pixel(150, 150), 0x808080FFu, 4));
    // the edges: pixel centres on integers in Direct3D, so the quad covers pixels 100..199
    ASSERT(near_rgb(pixel(100, 100), 0x808080FFu, 4));
    ASSERT(near_rgb(pixel(199, 199), 0x808080FFu, 4));
    ASSERT(near_rgb(pixel(200, 150), 0x204080FFu, 4));
    ASSERT(near_rgb(pixel(99, 150), 0x204080FFu, 4));
    // a colour: red times the grey
    f_quad(&g, 300, 100, 400, 200, 0.5f, 0xFF0000FFu);
    t_kick(&g);
    ASSERT(near_rgb(pixel(350, 150), 0x800000FFu, 4));
    // the frame finished: shown
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(&g);
    ASSERT_EQ(1u, g_frames_seen);
    ASSERT_EQ(640u, g_frame_w);
    ASSERT_EQ(480u, g_frame_h);
    ASSERT(near_rgb(shown(150, 150), 0x808080FFu, 4));
    ASSERT(near_rgb(shown(350, 150), 0x800000FFu, 4));
    pixel_done(&g);
    PASS();
}

TEST depth_test_keeps_the_nearer(void) {
    TGuest g;

    ASSERT(pixel_device(&g) != NULL);
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF000000u);
    // red at depth 0.3, then green over it at 0.7 (LEQUAL: behind, not drawn), blue at 0.1 (drawn)
    f_quad(&g, 100, 100, 200, 200, 0.3f, 0xFF0000FFu);
    f_quad(&g, 150, 150, 250, 250, 0.7f, 0xFF00FF00u);
    f_quad(&g, 180, 180, 220, 220, 0.1f, 0xFFFF0000u);
    t_kick(&g);
    ASSERT(near_rgb(pixel(160, 160), 0x800000FFu, 4));
    ASSERT(near_rgb(pixel(240, 240), 0x008000FFu, 4));
    ASSERT(near_rgb(pixel(190, 190), 0x000080FFu, 4));
    pixel_done(&g);
    PASS();
}

TEST two_combiner_stages_with_their_constants(void) {
    TGuest g;

    ASSERT(pixel_device(&g) != NULL);
    white_texture(&g);
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF000000u);
    // r1 = t0 * c0 + v0 * c1: white * (255, 128, 0) + grey vertex (128) * (0, 0, 128)
    f_combiner_two_stages(&g, 0xFFFF8000u, 0xFF000080u);
    f_quad(&g, 0, 0, 64, 64, 0.5f, 0xFF808080u);
    t_kick(&g);
    ASSERT(near_rgb(pixel(32, 32), 0xFF8040FFu, 4));
    pixel_done(&g);
    PASS();
}

TEST alpha_blending(void) {
    TGuest g;

    ASSERT(pixel_device(&g) != NULL);
    white_texture(&g);
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF000000u);
    f_quad(&g, 0, 0, 64, 64, 0.5f, 0xFF0000FFu); // red
    // half-transparent blue over it: src alpha, 1 - src alpha
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_SFACTOR, WL_NV2A_BLEND_SRC_ALPHA);
    t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR, WL_NV2A_BLEND_ONE_MINUS_SRC_ALPHA);
    t_m1(&g, WL_NV2A_K_SET_BLEND_EQUATION, WL_NV2A_BLEND_EQ_ADD);
    t_m1(&g, WL_NV2A_K_SET_DEPTH_TEST_ENABLE, 0);
    f_quad(&g, 0, 0, 64, 64, 0.5f, 0x80FF0000u);
    t_kick(&g);
    ASSERT(near_rgb(pixel(32, 32), 0x7F0080FFu, 4));
    pixel_done(&g);
    PASS();
}

TEST perspective_triangle(void) {
    TGuest g;

    ASSERT(pixel_device(&g) != NULL);
    white_texture(&g);
    f_perspective(&g);
    f_clear(&g, 0xFF000000u);
    // a triangle at w 2..4 around the screen's middle: (0,0,2) the middle, (1,1,4) at (400,180),
    // (-1,1,4) at (240,180)
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    f_vertex(&g, 0, 0, 2, 1, 0, 0, 0xFF00FF00u);
    f_vertex(&g, 1, 1, 4, 1, 1, 0, 0xFF00FF00u);
    f_vertex(&g, -1, 1, 4, 1, 0, 1, 0xFF00FF00u);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT(near_rgb(pixel(320, 200), 0x00FF00FFu, 4));
    ASSERT(near_rgb(pixel(320, 260), 0x000000FFu, 0));
    ASSERT(near_rgb(pixel(420, 200), 0x000000FFu, 0));
    pixel_done(&g);
    PASS();
}

// Direct3D's default culling (D3DCULL_CCW: front faces CW, the back culled) keeps what is wound
// clockwise on the screen: a title's screen-space quads (its logo, its menu) are, and were culled
// when the NV2A's CW was taken as OpenGL's y-up sense.
TEST direct3d_culling_keeps_quads_wound_clockwise_on_the_screen(void) {
    TGuest g;

    ASSERT(pixel_device(&g) != NULL);
    white_texture(&g);
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF000000u);
    t_m1(&g, WL_NV2A_K_SET_CULL_FACE_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_CULL_FACE, WL_NV2A_CULL_BACK);
    t_m1(&g, WL_NV2A_K_SET_FRONT_FACE, WL_NV2A_FRONT_CW);
    // clockwise on the screen (top left, top right, bottom right, bottom left): drawn
    f_quad(&g, 100, 100, 200, 200, 0.5f, 0xFFFFFFFFu);
    // counter-clockwise: culled
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_QUADS);
    f_vertex(&g, 300, 100, 0, 1, 0, 0, 0xFFFFFFFFu);
    f_vertex(&g, 300, 200, 0, 1, 0, 1, 0xFFFFFFFFu);
    f_vertex(&g, 400, 200, 0, 1, 1, 1, 0xFFFFFFFFu);
    f_vertex(&g, 400, 100, 0, 1, 1, 0, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT(near_rgb(pixel(150, 150), 0xFFFFFFFFu, 4));
    ASSERT(near_rgb(pixel(350, 150), 0x000000FFu, 0));
    // culling the front instead: the other way round
    f_clear(&g, 0xFF000000u);
    t_m1(&g, WL_NV2A_K_SET_CULL_FACE, WL_NV2A_CULL_FRONT);
    f_quad(&g, 100, 100, 200, 200, 0.5f, 0xFFFFFFFFu);
    t_kick(&g);
    ASSERT(near_rgb(pixel(150, 150), 0x000000FFu, 0));
    pixel_done(&g);
    PASS();
}

// mov oPos, v0; mov oD0, v3; mov oD1, v4; mov oT0, v9 (the end)
static const uint32_t f_two_colours_program[][4] = {
    {0x00000000, 0x0020001B, 0x0836006C, 0x0000F800},
    {0x00000000, 0x0020061B, 0x0836006C, 0x0000F818},
    {0x00000000, 0x0020081B, 0x0836006C, 0x0000F820},
    {0x00000000, 0x0020121B, 0x0836006C, 0x0000F849},
};

// A glow's program, whose final product E*F finds every TEV register holding a live value
// (tests/xbox/test_xbox_psh.c's a_final_product_with_every_register_held): drawn by its own TEV
// program, not texture 0 times the colour.
TEST a_final_product_with_every_register_held_is_drawn(void) {
    TGuest g;
    WlNv2aGxStats stats;
    unsigned i;

    ASSERT(pixel_device(&g) != NULL);
    // t0 grey (128): r0 = t0^2, r1 = t0^4, E*F = t0^6 (about 4/255)
    for (i = 0; i < 16; i++) {
        t_w32(&g, F_TEXTURE + 4 * i, 0xFF808080u);
    }
    f_program(&g, 0, f_two_colours_program, 4);
    f_clear(&g, 0xFF000000u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_COLOR_ICW, 0x08080000u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_COLOR_OCW, 0x000000C0u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_COLOR_ICW + 4, 0x0C0C0000u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_COLOR_OCW + 4, 0x000000D0u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_COLOR_ICW + 8, 0x04082415u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_COLOR_OCW + 8, 0x00000045u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_ALPHA_ICW, 0);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_ALPHA_OCW, 0);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW0, 0x050F0004u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_SPECULAR_FOG_CW1, 0x0C0D1400u);
    t_m1(&g, WL_NV2A_K_SET_COMBINER_CONTROL, 3);
    // diffuse (64, 128, 255) alpha 255, specular alpha 255 (its colour unread)
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_QUADS);
    for (i = 0; i < 4; i++) {
        float x = (i == 1 || i == 2) ? 64.0f : 0.0f, y = i >= 2 ? 64.0f : 0.0f;

        t_m1(&g, WL_NV2A_K_SET_VERTEX_DATA4UB + 4 * 4, 0xFF000000u);
        f_vertex(&g, x, y, 0.5f * 16777215.0f, 1, x / 64.0f, y / 64.0f, 0xFFFF8040u);
    }
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    wl_nv2a_gx_stats(g_gx, &stats);
    ASSERT_EQ(0u, stats.programs_unusable);
    // v0 t0 + (1 - v0) v1.a t0^6: (32 + 191 * 4/255, 64 + 127 * 4/255, 128 + 0): about (35, 66, 128)
    ASSERT(near_rgb(pixel(32, 32), 0x234280FFu, 6));
    pixel_done(&g);
    PASS();
}

// A second pass over part of the same geometry (some of the base pass's triangles, the same
// vertices) with the depth test LEQUAL and no depth write, a shadow or a light blended over a base
// pass: on the NV2A its depths equal the base pass's, so it passes everywhere it covers; on GX it
// must too, in every frame. The perspective projection's depth a + b / w was fitted from each
// draw's own vertices, so the two passes' fits differed in their rounding and the second failed
// the test over some or all of the first (Halo's ring flickered between shadowed and bright).
TEST a_second_pass_over_the_same_geometry_passes_lequal(void) {
    TGuest g;
    unsigned i, pass;
    // (x, y, z) of two triangles reaching far into the distance, w from 1.37 to 61.9: the base
    // pass draws both, the second pass the second
    static const float corners[6][3] = {{-1.73f, -0.61f, 1.37f}, {37.1f, -9.3f, 61.9f}, {-29.3f, 21.7f, 43.3f},
                                        {-29.3f, 21.7f, 43.3f},  {37.1f, -9.3f, 61.9f}, {13.7f, 29.1f, 7.31f}};

    ASSERT(pixel_device(&g) != NULL);
    white_texture(&g);
    // the clip program's transform with Direct3D's depth: z' = A z + B, w = z (near 0.1, far 1000)
    f_perspective(&g);
    f_constant(&g, 98, 0, 0, 1000.0f / 999.9f, -100.0f / 999.9f);
    f_clear(&g, 0xFF000000u);
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            // blue added over the red where the depth is not greater
            t_m1(&g, WL_NV2A_K_SET_DEPTH_MASK, 0);
            t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 1);
            t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_SFACTOR, WL_NV2A_BLEND_ONE);
            t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR, WL_NV2A_BLEND_ONE);
            t_m1(&g, WL_NV2A_K_SET_BLEND_EQUATION, WL_NV2A_BLEND_EQ_ADD);
        }
        t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
        for (i = pass ? 3u : 0u; i < 6u; i++) {
            f_vertex(&g, corners[i][0], corners[i][1], corners[i][2], 1, 0, 0, pass ? 0xFFFF0000u : 0xFF0000FFu);
        }
        t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
        t_kick(&g);
    }
    ASSERT(g_last_draw.pixels > 1000u);
    ASSERT_EQ(0u, g_last_draw.z_failed);
    ASSERT_EQ(g_last_draw.pixels, g_last_draw.written);
    pixel_done(&g);
    PASS();
}

// The same second pass with draws of six other projections between it and its base pass (the
// sky's, a view model's, another camera's: their depth ranges unlike the base pass's), and drawn
// by another program that computes the same position. A fit kept by recency (four of them) had
// let the base pass's go by then, and the second pass's own fit, from other vertices, differed in
// its rounding; the fit is now kept by the transform it was made for, which the second pass shares.
TEST a_second_pass_after_other_projections_passes_lequal(void) {
    TGuest g;
    unsigned i, other;
    static const float corners[6][3] = {{-1.73f, -0.61f, 1.37f}, {37.1f, -9.3f, 61.9f}, {-29.3f, 21.7f, 43.3f},
                                        {-29.3f, 21.7f, 43.3f},  {37.1f, -9.3f, 61.9f}, {13.7f, 29.1f, 7.31f}};
    // the clip program again with its instructions in another order: the same position
    static const uint32_t clip_again[][4] = {
        {0x00000000, 0x00EC601B, 0x0836186C, 0x01C00000}, // dp4 r12.w, v0, c[3]
        {0x00000000, 0x00EC401B, 0x0836186C, 0x02C00000}, // dp4 r12.z, v0, c[2]
        {0x00000000, 0x00EC001B, 0x0836186C, 0x08C00000}, // dp4 r12.x, v0, c[0]
        {0x00000000, 0x00EC201B, 0x0836186C, 0x04C00000}, // dp4 r12.y, v0, c[1]
        {0x00000000, 0x0600001B, 0x003603FF, 0x10180000}, // ircc r1.x, r12.w
        {0x00000000, 0x0040001B, 0xC400286C, 0x0EC00000}, // mul r12.xyz, r12, r1.x
        {0x00000000, 0x0027601B, 0x0C36006C, 0x0F200000}, // mov r2, c[-37]
        {0x00000000, 0x0087401B, 0xC436186C, 0x9000E800}, // mad oPos.xyz, r12, c[-38], r2
        {0x00000000, 0x0020061B, 0x0836006C, 0x0000F818}, // mov oD0, v3
        {0x00000000, 0x0020121B, 0x0836006C, 0x0000F849}, // mov oT0, v9
    };

    ASSERT(pixel_device(&g) != NULL);
    white_texture(&g);
    f_perspective(&g);
    f_constant(&g, 98, 0, 0, 1000.0f / 999.9f, -100.0f / 999.9f);
    f_clear(&g, 0xFF000000u);
    // the base pass: both triangles, writing depth
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    for (i = 0; i < 6u; i++) {
        f_vertex(&g, corners[i][0], corners[i][1], corners[i][2], 1, 0, 0, 0xFF0000FFu);
    }
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    // six other projections (near 0.2 .. 0.7, far 300 .. 800), a small triangle each at the left,
    // their depth not written
    t_m1(&g, WL_NV2A_K_SET_DEPTH_MASK, 0);
    for (other = 0; other < 6u; other++) {
        float near = 0.2f + 0.1f * (float)other, far = 300.0f + 100.0f * (float)other;

        f_constant(&g, 98, 0, 0, far / (far - near), -near * far / (far - near));
        t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
        f_vertex(&g, -3.0f, -1.0f, 2.0f + (float)other, 1, 0, 0, 0xFF00FF00u);
        f_vertex(&g, -2.0f, 1.0f, 30.0f, 1, 0, 0, 0xFF00FF00u);
        f_vertex(&g, -3.0f, 1.0f, 9.0f, 1, 0, 0, 0xFF00FF00u);
        t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    }
    // the second pass: the base pass's projection again, the other program, blue added where the
    // depth is not greater
    f_constant(&g, 98, 0, 0, 1000.0f / 999.9f, -100.0f / 999.9f);
    f_program(&g, 40, clip_again, 10);
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_SFACTOR, WL_NV2A_BLEND_ONE);
    t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR, WL_NV2A_BLEND_ONE);
    t_m1(&g, WL_NV2A_K_SET_BLEND_EQUATION, WL_NV2A_BLEND_EQ_ADD);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    for (i = 3; i < 6u; i++) {
        f_vertex(&g, corners[i][0], corners[i][1], corners[i][2], 1, 0, 0, 0xFFFF0000u);
    }
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    ASSERT(g_last_draw.pixels > 1000u);
    ASSERT_EQ(0u, g_last_draw.z_failed);
    ASSERT_EQ(g_last_draw.pixels, g_last_draw.written);
    {
        // (by the fit kept for the base pass's transform, which the other program's draw shares)
        bool shared = false;
        WlNv2aGxStats stats;

        ASSERT(wl_nv2a_gx_last_projection(g_gx, NULL, NULL, &shared));
        ASSERT(shared);
        wl_nv2a_gx_stats(g_gx, &stats);
        ASSERT_EQ(8u, stats.perspective_draws);
        ASSERT_EQ(1u, stats.fits_shared);
    }
    pixel_done(&g);
    PASS();
}

// a frame into the back buffer at `color`, shown when finished
static void frame_into(TGuest *g, uint32_t color) {
    t_m1(g, WL_NV2A_K_SET_SURFACE_COLOR_OFFSET, color);
    f_clear(g, 0xFF000000u);
}

static void frame_finished(TGuest *g, uint32_t color) {
    t_m1(g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(g);
    t_mmio(g, WL_NV2A_PCRTC_START, color);
}

// A title that samples a picture it showed (the surface it flipped to, as a linear 640x480
// texture), as Halo's sun glare reads the last frame's alpha: on the NV2A that picture is in
// memory, on GX it was only in the EFB. Once a displayed surface is read, each finished back buffer
// is copied into a target at its address, and the next frame samples it.
TEST a_picture_shown_is_sampled_the_next_frame(void) {
    TGuest g;
    unsigned frame;
    enum { F_COLOR2 = 0x180000 };

    ASSERT(pixel_device(&g) != NULL);
    white_texture(&g);
    f_program(&g, 0, f_screen_program, 3);
    t_m1(&g, WL_NV2A_K_SET_DEPTH_TEST_ENABLE, 0);
    // frames into F_COLOR and F_COLOR2 by turns: a quad of the frame's colour at (100, 100), and
    // from the second on a quad at (300, 100) sampling the picture shown before
    for (frame = 0; frame < 3; frame++) {
        uint32_t back = frame & 1u ? (uint32_t)F_COLOR2 : (uint32_t)F_COLOR, shown = back == F_COLOR ? F_COLOR2 : F_COLOR;
        uint32_t colour = frame == 1 ? 0xFF00FF00u : 0xFF0000FFu; // (ABGR: green, then red)

        frame_into(&g, back);
        f_texture(&g, F_TEXTURE, WL_NV2A_TEX_SZ_A8R8G8B8, 2, 2);
        f_quad(&g, 100, 100, 200, 200, 0.5f, colour);
        if (frame > 0) {
            t_m1(&g, WL_NV2A_K_SET_TEXTURE_OFFSET, shown);
            t_m1(&g, WL_NV2A_K_SET_TEXTURE_FORMAT, 1u | 2u << 4 | (uint32_t)WL_NV2A_TEX_LU_A8R8G8B8 << 8 | 1u << 16);
            t_m1(&g, WL_NV2A_K_SET_TEXTURE_CONTROL1, 2560u << 16);
            t_m1(&g, WL_NV2A_K_SET_TEXTURE_IMAGE_RECT, 640u << 16 | 480u);
            // (a linear texture's coordinates are texels: the quad shows the picture's (100, 100)-(200, 200))
            t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_QUADS);
            f_vertex(&g, 300, 100, 0, 1, 100, 100, 0xFFFFFFFFu);
            f_vertex(&g, 400, 100, 0, 1, 200, 100, 0xFFFFFFFFu);
            f_vertex(&g, 400, 200, 0, 1, 200, 200, 0xFFFFFFFFu);
            f_vertex(&g, 300, 200, 0, 1, 100, 200, 0xFFFFFFFFu);
            t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
        }
        t_kick(&g);
        if (frame == 2) {
            // the third frame shows the second's green quad (the first, read before any picture was
            // kept, is not looked at)
            ASSERT(near_rgb(pixel(150, 150), 0xFF0000FFu, 4));
            ASSERT(near_rgb(pixel(350, 150), 0x00FF00FFu, 4));
        }
        frame_finished(&g, back);
    }
    pixel_done(&g);
    PASS();
}

typedef struct Sink {
    uint8_t *data;
    size_t size, capacity;
} Sink;

static WlResult sink_write(void *user, const void *data, uint32_t bytes) {
    Sink *s = user;

    if (s->size + bytes > s->capacity) {
        size_t capacity = (s->size + bytes) * 2;
        uint8_t *grown = realloc(s->data, capacity);

        if (grown == NULL) {
            return WL_ERR_NOMEM;
        }
        s->data = grown;
        s->capacity = capacity;
    }
    memcpy(s->data + s->size, data, bytes);
    s->size += bytes;
    return WL_OK;
}

static WlNv2aGx *g_replayed;

static void replay_created(void *user, WlNv2a *dev) {
    (void)user;
    wl_nv2a_gx_attach(g_replayed, dev);
}

// A frame drawn live with a capture running, then the capture replayed through a fresh model, GX
// backend and software GX: the same picture. With WL_NV2A_CAPTURE_OUT set, the capture is written
// there too (the replay tool's test reads it).
TEST capture_replays_to_the_same_pixels(void) {
    TGuest g;
    Sink s;
    WlNv2aCaptureSink sink;
    WlNv2aReplayHooks hooks;
    WlNv2aReplayInfo info;
    WlNv2aBackend backend;
    WlGxSoftConfig cfg;
    uint8_t *live;
    uint32_t w, h;
    const char *out = getenv("WL_NV2A_CAPTURE_OUT");

    memset(&s, 0, sizeof s);
    ASSERT(pixel_device(&g) != NULL);
    f_program(&g, 0, f_screen_program, 3);
    t_kick(&g);
    sink.user = &s;
    sink.write = sink_write;
    ASSERT_EQ(WL_OK, wl_nv2a_capture_begin(g.dev, &sink));
    f_clear(&g, 0xFF102030u);
    f_quad(&g, 10, 10, 300, 200, 0.5f, 0xFF40C0FFu);
    f_combiner_two_stages(&g, 0xFF80FF80u, 0xFF200000u);
    f_quad(&g, 200, 100, 500, 400, 0.4f, 0xFFFFFFFFu);
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(&g);
    t_mmio(&g, WL_NV2A_PCRTC_START, F_COLOR);
    wl_nv2a_capture_end(g.dev);
    ASSERT_EQ(1u, g_frames_seen);
    ASSERT(g_frame != NULL);
    w = g_frame_w;
    h = g_frame_h;
    live = g_frame;
    g_frame = NULL;
    wl_nv2a_gx_destroy(g_gx);
    g_gx = NULL;
    t_destroy(&g);

    // (a fresh software GX and backend)
    memset(&cfg, 0, sizeof cfg);
    cfg.frame = on_frame;
    g_frames_seen = 0;
    ASSERT_EQ(WL_OK, wl_gxsoft_attach(&cfg));
    g_replayed = wl_nv2a_gx_create(NULL);
    ASSERT(g_replayed != NULL);
    wl_nv2a_gx_backend(g_replayed, &backend);
    memset(&hooks, 0, sizeof hooks);
    hooks.created = replay_created;
    ASSERT_EQ(WL_OK, wl_nv2a_replay(s.data, s.size, &backend, &hooks, &info));
    ASSERT_EQ(1u, g_frames_seen);
    ASSERT(g_frame != NULL);
    ASSERT_EQ(w, g_frame_w);
    ASSERT_EQ(h, g_frame_h);
    ASSERT_EQ(0, memcmp(live, g_frame, (size_t)w * h * 4u));
    ASSERT_EQ(2u, info.stats.draws);
    ASSERT_EQ(1u, info.flips);
    // (what was drawn: the second quad's two stages over the first's texture times colour)
    ASSERT(!near_rgb(shown(250, 150), 0x102030FFu, 8));
    if (out != NULL) {
        FILE *f = fopen(out, "wb");

        ASSERT(f != NULL);
        ASSERT_EQ(s.size, fwrite(s.data, 1, s.size, f));
        fclose(f);
    }
    wl_nv2a_gx_destroy(g_replayed);
    g_replayed = NULL;
    wl_gxsoft_detach();
    wl_gx_shutdown();
    free(live);
    free(g_frame);
    g_frame = NULL;
    free(s.data);
    PASS();
}

// ---- the transform unit's draws ----------------------------------------------------------------------

// the clip program with its colour squared: the processor's (not affine), the position the same
static const uint32_t f_clip_program_squared[][4] = {
    {0x00000000, 0x00EC001B, 0x0836186C, 0x08C00000}, // dp4 r12.x, v0, c[0]
    {0x00000000, 0x00EC201B, 0x0836186C, 0x04C00000}, // dp4 r12.y, v0, c[1]
    {0x00000000, 0x00EC401B, 0x0836186C, 0x02C00000}, // dp4 r12.z, v0, c[2]
    {0x00000000, 0x00EC601B, 0x0836186C, 0x01C00000}, // dp4 r12.w, v0, c[3]
    {0x00000000, 0x0600001B, 0x003603FF, 0x10180000}, // ircc r1.x, r12.w
    {0x00000000, 0x0040001B, 0xC400286C, 0x0EC00000}, // mul r12.xyz, r12, r1.x
    {0x00000000, 0x0027601B, 0x0C36006C, 0x0F200000}, // mov r2, c[-37]
    {0x00000000, 0x0087401B, 0xC436186C, 0x9000E800}, // mad oPos.xyz, r12, c[-38], r2
    {0x00000000, 0x0040061B, 0x0836106C, 0x0000F818}, // mul oD0, v3, v3
    {0x00000000, 0x0020121B, 0x0836006C, 0x0000F849}, // mov oT0, v9
};

enum { XF_ARRAY = 0x70000, XF_VERTICES = 12 };

// twelve vertices of four triangles reaching from w 1.4 to 60 (position F3, colour D3DCOLOR,
// texture coordinate F2: 24 bytes a vertex), drawn from the arrays
static void xf_arrays(TGuest *g) {
    static const float corners[XF_VERTICES][3] = {
        {-1.73f, -0.61f, 1.37f}, {37.1f, -9.3f, 59.9f},  {-29.3f, 21.7f, 43.3f}, {-29.3f, 21.7f, 43.3f},
        {37.1f, -9.3f, 59.9f},   {13.7f, 29.1f, 7.31f},  {-3.1f, -2.2f, 2.5f},   {4.4f, -2.9f, 5.1f},
        {0.3f, 3.3f, 3.9f},      {-7.1f, 1.9f, 9.2f},    {-2.2f, -5.5f, 4.0f},   {-1.1f, 0.7f, 1.9f}};
    unsigned v;

    for (v = 0; v < XF_VERTICES; v++) {
        uint32_t at = XF_ARRAY + 24u * v;

        t_w32(g, at, t_f(corners[v][0]));
        t_w32(g, at + 4, t_f(corners[v][1]));
        t_w32(g, at + 8, t_f(corners[v][2]));
        t_w32(g, at + 12, 0xFF000000u | (v * 20u) << 16 | (255u - v * 20u) << 8 | 0x60u);
        t_w32(g, at + 16, t_f((float)(v % 3) * 0.5f));
        t_w32(g, at + 20, t_f((float)(v % 2)));
    }
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT, 24u << 8 | 3u << 4 | WL_NV2A_VTX_F);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET, XF_ARRAY);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * 3, 24u << 8 | 4u << 4 | WL_NV2A_VTX_UB_D3D);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * 3, XF_ARRAY + 12);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * 9, 24u << 8 | 2u << 4 | WL_NV2A_VTX_F);
    t_m1(g, WL_NV2A_K_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * 9, XF_ARRAY + 16);
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    t_m1(g, WL_NV2A_K_DRAW_ARRAYS, (uint32_t)(XF_VERTICES - 1) << 24);
    t_m1(g, WL_NV2A_K_SET_BEGIN_END, 0);
}

// the EFB after the arrays' draw, with or without the transform unit; a second pass by the squared
// program (depth mask off, LEQUAL) when asked, which must pass wherever it draws
static int xf_frame(uint32_t *efb, bool no_xf, bool second_pass, WlNv2aGxStats *stats) {
    TGuest g;
    int x, y;

    g_no_xf = no_xf;
    if (pixel_device(&g) == NULL) {
        g_no_xf = false;
        return 0;
    }
    g_no_xf = false;
    f_perspective(&g);
    f_constant(&g, 98, 0, 0, 1000.0f / 999.9f, -100.0f / 999.9f);
    f_clear(&g, 0xFF000000u);
    xf_arrays(&g);
    t_kick(&g);
    if (second_pass) {
        f_program(&g, 8, f_clip_program_squared, 10);
        t_m1(&g, WL_NV2A_K_SET_DEPTH_MASK, 0);
        t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
        t_m1(&g, WL_NV2A_K_DRAW_ARRAYS, (uint32_t)(XF_VERTICES - 1) << 24);
        t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
        t_kick(&g);
    }
    for (y = 0; y < 480; y++) {
        for (x = 0; x < 640; x++) {
            efb[y * 640 + x] = pixel(x, y);
        }
    }
    wl_nv2a_gx_stats(g_gx, stats);
    pixel_done(&g);
    return 1;
}

TEST arrays_through_the_transform_unit_land_as_on_the_processor(void) {
    static uint32_t cpu[640 * 480], xf[640 * 480];
    WlNv2aGxStats cpu_stats, xf_stats;
    unsigned i, covered = 0, beyond = 0;

    ASSERT(xf_frame(cpu, true, false, &cpu_stats));
    ASSERT(xf_frame(xf, false, false, &xf_stats));
    ASSERT_EQ(0u, cpu_stats.xf_draws);
    // (position, colour passed, coordinate passed: all of it the transform unit's)
    ASSERT_EQ(1u, xf_stats.xf_draws);
    for (i = 0; i < 640u * 480u; i++) {
        if ((cpu[i] & 0xFFFFFF00u) != 0) {
            covered++;
        }
        if (!near_rgb(cpu[i], xf[i], 8)) {
            beyond++;
        }
    }
    ASSERT(covered > 20000u);
    // (the edges' pixels may fall the other way)
    ASSERTm("pixels beyond the tolerance", beyond * 200u < covered);
    PASS();
}

TEST a_second_pass_by_the_processor_passes_over_the_transform_units_depths(void) {
    static uint32_t efb[640 * 480];
    WlNv2aGxStats stats;
    uint32_t cpu_failed, cpu_pixels, xf_failed, xf_pixels;

    // (the triangles cover each other: where a nearer one is, the farther fails on the processor too)
    ASSERT(xf_frame(efb, true, true, &stats));
    cpu_failed = g_last_draw.z_failed;
    cpu_pixels = g_last_draw.pixels;
    ASSERT(xf_frame(efb, false, true, &stats));
    xf_failed = g_last_draw.z_failed;
    xf_pixels = g_last_draw.pixels;
    ASSERT_EQ(1u, stats.xf_draws);
    ASSERT_EQ(1u, stats.xf_position_draws);
    ASSERT(xf_pixels > 20000u);
    // (the pass over the transform unit's depths fails where the processor's does, and nowhere else
    // but at the edges)
    ASSERT((xf_failed > cpu_failed ? xf_failed - cpu_failed : cpu_failed - xf_failed) * 200u < cpu_pixels);
    ASSERT((xf_pixels > cpu_pixels ? xf_pixels - cpu_pixels : cpu_pixels - xf_pixels) * 200u < cpu_pixels);
    PASS();
}

// the arrays' draw twice in a frame, the second time after the guest rewrote a vertex, each with or
// without the vertex cache: the EFB after each
static int xf_cached_frames(uint32_t *first, uint32_t *second, size_t cache, WlNv2aGxStats *stats,
                            WlNv2aVCacheStats *vstats) {
    TGuest g;
    int x, y;

    g_vertex_cache = cache;
    if (pixel_device(&g) == NULL) {
        g_vertex_cache = 0;
        return 0;
    }
    g_vertex_cache = 0;
    f_perspective(&g);
    f_constant(&g, 98, 0, 0, 1000.0f / 999.9f, -100.0f / 999.9f);
    f_clear(&g, 0xFF000000u);
    xf_arrays(&g);
    t_kick(&g);
    for (y = 0; y < 480; y++) {
        for (x = 0; x < 640; x++) {
            first[y * 640 + x] = pixel(x, y);
        }
    }
    // (the next frame: a corner moved, a colour changed; the cache checks the arrays again)
    wl_nv2a_gx_present(g_gx);
    f_clear(&g, 0xFF000000u);
    t_w32(&g, XF_ARRAY + 24u * 5u, t_f(19.5f));
    t_w32(&g, XF_ARRAY + 24u * 7u + 12u, 0xFF20C040u);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    t_m1(&g, WL_NV2A_K_DRAW_ARRAYS, (uint32_t)(XF_VERTICES - 1) << 24);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    for (y = 0; y < 480; y++) {
        for (x = 0; x < 640; x++) {
            second[y * 640 + x] = pixel(x, y);
        }
    }
    wl_nv2a_gx_stats(g_gx, stats);
    wl_nv2a_vcache_stats(wl_nv2a_gx_vertex_cache(g_gx), vstats);
    pixel_done(&g);
    return 1;
}

TEST arrays_kept_for_gx_to_index_draw_to_the_bit(void) {
    static uint32_t sent[2][640 * 480], kept[2][640 * 480];
    WlNv2aGxStats sent_stats, kept_stats;
    WlNv2aVCacheStats sent_vstats, kept_vstats;

    ASSERT(xf_cached_frames(sent[0], sent[1], 0, &sent_stats, &sent_vstats));
    ASSERT(xf_cached_frames(kept[0], kept[1], 64u * 1024u, &kept_stats, &kept_vstats));
    ASSERT_EQ(0u, sent_stats.indexed_draws);
    ASSERT_EQ(2u, sent_stats.xf_draws);
    // (position, colour and coordinate all from arrays: each frame's draw sent as indices)
    ASSERT_EQ(2u, kept_stats.indexed_draws);
    ASSERT_EQ(2u * XF_VERTICES, kept_stats.indexed_vertices);
    ASSERT_EQ(3u, kept_vstats.converted);
    ASSERT_EQ(3u, kept_vstats.checks);
    ASSERT(kept_vstats.reconverted >= 2u);
    ASSERT_MEM_EQ(sent[0], kept[0], sizeof sent[0]);
    ASSERT_MEM_EQ(sent[1], kept[1], sizeof sent[1]);
    ASSERT(memcmp(sent[0], sent[1], sizeof sent[0]) != 0);
    PASS();
}

SUITE(pixels_suite) {
    RUN_TEST(console_suite_on_the_software_gx);
    RUN_TEST(textured_quad_through_the_combiners);
    RUN_TEST(depth_test_keeps_the_nearer);
    RUN_TEST(two_combiner_stages_with_their_constants);
    RUN_TEST(alpha_blending);
    RUN_TEST(perspective_triangle);
    RUN_TEST(direct3d_culling_keeps_quads_wound_clockwise_on_the_screen);
    RUN_TEST(a_final_product_with_every_register_held_is_drawn);
    RUN_TEST(a_second_pass_over_the_same_geometry_passes_lequal);
    RUN_TEST(a_second_pass_after_other_projections_passes_lequal);
    RUN_TEST(a_picture_shown_is_sampled_the_next_frame);
    RUN_TEST(capture_replays_to_the_same_pixels);
    RUN_TEST(arrays_through_the_transform_unit_land_as_on_the_processor);
    RUN_TEST(a_second_pass_by_the_processor_passes_over_the_transform_units_depths);
    RUN_TEST(arrays_kept_for_gx_to_index_draw_to_the_bit);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(pixels_suite);
    GREATEST_MAIN_END();
}
