// SPDX-License-Identifier: MIT
// wl/gxsoft: the software GX drawing what wl/platform's host GX layer is asked, pixel by pixel. The
// fill rule at GX's sample point, culling by the screen's winding, the perspective projection's
// clipping at the near plane, the depth test and its place against the alpha test, blending and the
// EFB's formats, texture sampling (nearest, bilinear, repeat, CMPR, levels, one map read at two
// coordinates), copies out of the EFB and the parked back buffer put back, the display copy, and
// the state GX takes as it is flagged.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "wl/codec/gx_tex.h"
#include "wl/gxsoft/gxsoft.h"
#include "wl/platform/gx_direct.h"
#include "wl/platform/gx_target.h"
#include "wl/platform/host/gx_direct_host.h"
#include "wl/platform/platform.h"
#include "wl/platform/tev.h"

static uint8_t *g_frame;
static uint32_t g_frame_w, g_frame_h, g_frames;
static WlGxSoftDraw g_last;

static void on_frame(void *user, const uint8_t *rgba, uint32_t w, uint32_t h) {
    (void)user;
    free(g_frame);
    g_frame = malloc((size_t)w * h * 4u);
    memcpy(g_frame, rgba, (size_t)w * h * 4u);
    g_frame_w = w;
    g_frame_h = h;
    g_frames++;
}

static void on_draw(void *user, const WlGxSoftDraw *d) {
    (void)user;
    g_last = *d;
}

// GX up with the software GX, a frame begun over `clear`, the state a draw needs: an orthographic
// projection over the 640 x 480 pixels, no blending, no depth test, culling off, vertex colours
static void begin(uint32_t clear, bool efb_alpha) {
    WlGxSoftConfig cfg;
    WlTevProgram tev;
    float m[4][4];

    memset(&cfg, 0, sizeof cfg);
    cfg.frame = on_frame;
    cfg.draw = on_draw;
    g_frames = 0;
    wl_gx_init();
    wl_host_gx_direct_reset();
    wl_gxsoft_attach(&cfg);
    wl_gx_set_efb_alpha(efb_alpha);
    wl_gx_begin_frame(clear);
    wl_gx_direct_begin();
    memset(m, 0, sizeof m);
    m[0][0] = 2.0f / 640.0f;
    m[0][3] = -1.0f;
    m[1][1] = -2.0f / 480.0f;
    m[1][3] = 1.0f;
    m[2][2] = 1.0f;
    m[3][3] = 1.0f;
    wl_gx_direct_projection((const float (*)[4])m, true);
    wl_gx_direct_viewport(0, 0, 640, 480);
    wl_gx_direct_scissor(0, 0, 640, 480);
    wl_gx_direct_blend(WL_GX_BM_NONE, WL_GX_BL_ONE, WL_GX_BL_ZERO);
    wl_gx_direct_zmode(false, WL_GX_ALWAYS, false);
    wl_gx_direct_zcompare_early(true);
    wl_gx_direct_alpha_compare(WL_GX_ALWAYS, 0);
    wl_gx_direct_color_update(true, true);
    wl_gx_direct_cull(WL_GX_CULL_NONE);
    wl_tev_recipe(&tev, WL_GX_RECIPE_VERTEX_COLOR);
    wl_tev_apply_unchecked(&tev);
}

static void end(void) {
    wl_gxsoft_detach();
    wl_gx_shutdown();
    free(g_frame);
    g_frame = NULL;
}

static void layout(uint8_t colors, uint8_t texcoords) {
    WlGxDirectLayout l = {colors, texcoords, 0, 0, 0, 0};
    wl_gx_direct_layout(&l);
}

static WlGxDirectVertex vtx(float x, float y, float z, uint32_t rgba, float s, float t) {
    WlGxDirectVertex v;

    memset(&v, 0, sizeof v);
    v.position[0] = x;
    v.position[1] = y;
    v.position[2] = z;
    v.color[0][0] = (uint8_t)(rgba >> 24);
    v.color[0][1] = (uint8_t)(rgba >> 16);
    v.color[0][2] = (uint8_t)(rgba >> 8);
    v.color[0][3] = (uint8_t)rgba;
    v.texcoord[0][0] = s;
    v.texcoord[0][1] = t;
    v.texcoord[1][0] = s * 0.5f;
    v.texcoord[1][1] = t;
    return v;
}

// a quad (x0, y0)-(x1, y1), clockwise on the screen, at depth z (0..1), one colour, the texture's
// coordinates 0..su by 0..sv
static void quad(float x0, float y0, float x1, float y1, float z, uint32_t rgba, float su, float sv) {
    WlGxDirectVertex v[4];

    v[0] = vtx(x0, y0, z - 1.0f, rgba, 0, 0);
    v[1] = vtx(x1, y0, z - 1.0f, rgba, su, 0);
    v[2] = vtx(x1, y1, z - 1.0f, rgba, su, sv);
    v[3] = vtx(x0, y1, z - 1.0f, rgba, 0, sv);
    wl_gx_direct_draw(WL_GX_DIRECT_QUADS, v, 4);
}

static int near_rgba(uint32_t got, uint32_t want, int slack) {
    int shift;

    for (shift = 0; shift < 32; shift += 8) {
        int d = (int)((got >> shift) & 0xFFu) - (int)((want >> shift) & 0xFFu);

        if (d > slack || d < -slack) {
            return 0;
        }
    }
    return 1;
}

static uint32_t px(int x, int y) {
    return wl_gx_peek_rgba(x, y);
}

// a texture of `w` x `h` RGBA8 pixels (rows of 0xRRGGBBAA) in `fmt`, with `mips` levels whose
// pixels `level_rgba` gives (NULL: only level 0)
static void *texture(WlGxTexObj *tex, WlGxTexFormat fmt, uint32_t w, uint32_t h, const uint32_t *rgba,
                     const uint32_t *level1, uint8_t wrap, uint8_t min, uint8_t mag) {
    size_t bytes0 = wl_gxc_tex_bytes((WlGxcFormat)fmt, w, h), bytes1 = level1 ? wl_gxc_tex_bytes((WlGxcFormat)fmt, w / 2, h / 2) : 0;
    uint8_t *image = aligned_alloc(32, (bytes0 + bytes1 + 31u) & ~(size_t)31u);
    uint8_t *pixels = malloc((size_t)w * h * 4u);
    WlGxTexDesc d;
    uint32_t i;

    for (i = 0; i < w * h; i++) {
        pixels[i * 4] = (uint8_t)(rgba[i] >> 24);
        pixels[i * 4 + 1] = (uint8_t)(rgba[i] >> 16);
        pixels[i * 4 + 2] = (uint8_t)(rgba[i] >> 8);
        pixels[i * 4 + 3] = (uint8_t)rgba[i];
    }
    wl_gxc_encode((WlGxcFormat)fmt, pixels, w, h, image, bytes0);
    if (level1 != NULL) {
        for (i = 0; i < (w / 2) * (h / 2); i++) {
            pixels[i * 4] = (uint8_t)(level1[i] >> 24);
            pixels[i * 4 + 1] = (uint8_t)(level1[i] >> 16);
            pixels[i * 4 + 2] = (uint8_t)(level1[i] >> 8);
            pixels[i * 4 + 3] = (uint8_t)level1[i];
        }
        wl_gxc_encode((WlGxcFormat)fmt, pixels, w / 2, h / 2, image + bytes0, bytes1);
    }
    free(pixels);
    memset(&d, 0, sizeof d);
    d.image = image;
    d.width = (uint16_t)w;
    d.height = (uint16_t)h;
    d.format = (uint8_t)fmt;
    d.mips = level1 != NULL ? 2u : 1u;
    d.sampler.wrap_s = d.sampler.wrap_t = wrap;
    d.sampler.min_filter = min;
    d.sampler.mag_filter = mag;
    wl_gx_tex_init_ex(tex, &d);
    return image;
}

static void texture_replace(void) {
    WlTevProgram tev;

    wl_tev_recipe(&tev, WL_GX_RECIPE_TEX_REPLACE);
    wl_tev_apply_unchecked(&tev);
}

TEST the_fill_rule_at_gx_sample_point(void) {
    int x, y, n = 0;

    begin(0x000000FFu, false);
    layout(1, 0);
    quad(10, 10, 20, 20, 0.5f, 0xFF0000FFu, 0, 0);
    for (y = 0; y < 40; y++) {
        for (x = 0; x < 40; x++) {
            n += px(x, y) == 0xFF0000FFu;
        }
    }
    // pixels 10..19 each way: a pixel is sampled at 7/12 past its corner
    ASSERT_EQ(100, n);
    ASSERT_EQ(0xFF0000FFu, px(10, 10));
    ASSERT_EQ(0xFF0000FFu, px(19, 19));
    ASSERT_EQ(0x000000FFu, px(20, 15));
    ASSERT_EQ(0x000000FFu, px(9, 15));
    ASSERT_EQ(100u, g_last.written);
    // two quads sharing an edge cover every pixel once
    quad(20, 10, 30, 20, 0.5f, 0x00FF00FFu, 0, 0);
    ASSERT_EQ(100u, g_last.written);
    ASSERT_EQ(0xFF0000FFu, px(19, 12));
    ASSERT_EQ(0x00FF00FFu, px(20, 12));
    end();
    PASS();
}

TEST culling_by_the_screen_winding(void) {
    WlGxDirectVertex v[3];

    begin(0x000000FFu, false);
    layout(1, 0);
    // clockwise on the screen: GX's front face, which GX_CULL_FRONT discards
    wl_gx_direct_cull(WL_GX_CULL_FRONT);
    quad(0, 0, 16, 16, 0.5f, 0xFFFFFFFFu, 0, 0);
    ASSERT_EQ(0x000000FFu, px(4, 4));
    ASSERT_EQ(2u, g_last.culled);
    wl_gx_direct_cull(WL_GX_CULL_BACK);
    quad(0, 0, 16, 16, 0.5f, 0xFFFFFFFFu, 0, 0);
    ASSERT_EQ(0xFFFFFFFFu, px(4, 4));
    // counter-clockwise: the back face
    v[0] = vtx(40, 0, -0.5f, 0x00FF00FFu, 0, 0);
    v[1] = vtx(40, 16, -0.5f, 0x00FF00FFu, 0, 0);
    v[2] = vtx(56, 0, -0.5f, 0x00FF00FFu, 0, 0);
    wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLES, v, 3);
    ASSERT_EQ(0x000000FFu, px(42, 2));
    wl_gx_direct_cull(WL_GX_CULL_FRONT);
    wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLES, v, 3);
    ASSERT_EQ(0x00FF00FFu, px(42, 2));
    // a strip's every other triangle is reversed, so all of its triangles face one way
    {
        WlGxDirectVertex s[4] = {vtx(100, 0, -0.5f, 0x0000FFFFu, 0, 0), vtx(100, 16, -0.5f, 0x0000FFFFu, 0, 0),
                                 vtx(116, 0, -0.5f, 0x0000FFFFu, 0, 0), vtx(116, 16, -0.5f, 0x0000FFFFu, 0, 0)};

        wl_gx_direct_cull(WL_GX_CULL_BACK);
        wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLE_STRIP, s, 4);
        ASSERT_EQ(2u, g_last.culled);
        wl_gx_direct_cull(WL_GX_CULL_FRONT);
        wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLE_STRIP, s, 4);
        ASSERT_EQ(0u, g_last.culled);
        ASSERT_EQ(0x0000FFFFu, px(102, 2));
        ASSERT_EQ(0x0000FFFFu, px(114, 14));
    }
    end();
    PASS();
}

// a perspective projection as GX keeps it: x' = 2/W X + Z, y' = -2/H Y - Z, z' = (1 - a) Z + b,
// w' = -Z; vertices (X, Y, Z) = (x w, y w, -w) for pixel (x, y) at distance w: depth a + b / w
static void perspective(float a, float b) {
    float m[4][4];

    memset(m, 0, sizeof m);
    m[0][0] = 2.0f / 640.0f;
    m[0][2] = 1.0f;
    m[1][1] = -2.0f / 480.0f;
    m[1][2] = -1.0f;
    m[2][2] = 1.0f - a;
    m[2][3] = b;
    m[3][2] = -1.0f;
    wl_gx_direct_projection((const float (*)[4])m, false);
}

TEST the_near_plane_clips(void) {
    WlGxDirectVertex v[3];

    begin(0x000000FFu, false);
    layout(1, 0);
    // depth 1 - 1/w: w 1 at the near plane, beyond it farther
    perspective(1.0f, -1.0f);
    v[0] = vtx(100 * 2, 100 * 2, -2, 0xFFFFFFFFu, 0, 0);
    v[1] = vtx(300 * 2, 100 * 2, -2, 0xFFFFFFFFu, 0, 0);
    v[2] = vtx(200 * 2, 300 * 2, -2, 0xFFFFFFFFu, 0, 0);
    wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLES, v, 3);
    ASSERT_EQ(0xFFFFFFFFu, px(200, 150));
    ASSERT_EQ(0u, g_last.clipped);
    // the same triangle with one corner nearer than the near plane (w 0.5): cut where it crosses
    v[2] = vtx(200 * 0.5f, 300 * 0.5f, -0.5f, 0xFF0000FFu, 0, 0);
    wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLES, v, 3);
    ASSERT_EQ(0u, g_last.clipped);
    ASSERT(g_last.written > 0u);
    ASSERT(g_last.y1 < 300.0f);
    // all of it behind the eye: nothing
    v[0].position[2] = v[1].position[2] = v[2].position[2] = 2.0f;
    wl_gx_direct_draw(WL_GX_DIRECT_TRIANGLES, v, 3);
    ASSERT_EQ(1u, g_last.clipped);
    ASSERT_EQ(0u, g_last.written);
    end();
    PASS();
}

TEST the_depth_test_and_its_place(void) {
    WlTevProgram tev;
    uint32_t texels[16];
    WlGxTexObj tex;
    void *image;
    unsigned i;

    begin(0x000000FFu, false);
    layout(1, 0);
    wl_gx_direct_zmode(true, WL_GX_LEQUAL, true);
    quad(0, 0, 32, 32, 0.3f, 0xFF0000FFu, 0, 0);
    quad(16, 16, 48, 48, 0.7f, 0x00FF00FFu, 0, 0);
    ASSERT_EQ(0xFF0000FFu, px(20, 20));
    ASSERT_EQ(0x00FF00FFu, px(40, 40));
    ASSERT_EQ(256u, g_last.z_failed);
    ASSERT_EQ((uint32_t)(0.3 * 16777215.0 + 0.5), wl_gxsoft_depth(10, 10));
    // without the test, no update either
    wl_gx_direct_zmode(false, WL_GX_ALWAYS, true);
    quad(0, 0, 8, 8, 0.9f, 0x0000FFFFu, 0, 0);
    ASSERT_EQ((uint32_t)(0.3 * 16777215.0 + 0.5), wl_gxsoft_depth(4, 4));
    // a texel the alpha test discards: depth written before texturing, not after
    for (i = 0; i < 16; i++) {
        texels[i] = 0xFFFFFF00u;
    }
    image = texture(&tex, WL_GX_TEX_RGBA8, 4, 4, texels, NULL, WL_GX_WRAP_CLAMP, WL_GX_FILTER_NEAREST,
                    WL_GX_FILTER_NEAREST);
    wl_gx_direct_texture(0, &tex, NULL, NULL);
    layout(1, 1);
    wl_tev_recipe(&tev, WL_GX_RECIPE_TEX_MODULATE);
    wl_tev_apply_unchecked(&tev);
    wl_gx_direct_alpha_compare(WL_GX_GREATER, 0);
    wl_gx_direct_zmode(true, WL_GX_ALWAYS, true);
    wl_gx_direct_zcompare_early(false);
    quad(100, 0, 110, 10, 0.1f, 0xFFFFFFFFu, 1, 1);
    ASSERT_EQ(100u, g_last.alpha_failed);
    ASSERT_EQ(0x00FFFFFFu, wl_gxsoft_depth(105, 5));
    wl_gx_direct_zcompare_early(true);
    quad(100, 0, 110, 10, 0.1f, 0xFFFFFFFFu, 1, 1);
    ASSERT_EQ((uint32_t)(0.1 * 16777215.0 + 0.5), wl_gxsoft_depth(105, 5));
    ASSERT_EQ(0x000000FFu, px(105, 5));
    free(image);
    end();
    PASS();
}

TEST blending_and_the_efb_formats(void) {
    begin(0x000000FFu, true);
    layout(1, 0);
    // RGBA6: six bits a channel, widened as a copy widens them
    quad(0, 0, 16, 16, 0.5f, 0x80402010u, 0, 0);
    ASSERT_EQ_FMT(0x82412010u, px(4, 4), "%08x");
    // half-transparent blue over red: src alpha, 1 - src alpha
    quad(0, 0, 16, 16, 0.5f, 0xFF0000FFu, 0, 0);
    wl_gx_direct_blend(WL_GX_BM_BLEND, WL_GX_BL_SRCALPHA, WL_GX_BL_INVSRCALPHA);
    quad(0, 0, 16, 16, 0.5f, 0x0000FF80u, 0, 0);
    ASSERT(near_rgba(px(4, 4), 0x7F0080BFu, 4));
    // additive: one, one
    wl_gx_direct_blend(WL_GX_BM_BLEND, WL_GX_BL_ONE, WL_GX_BL_ONE);
    quad(0, 0, 16, 16, 0.5f, 0x00400000u, 0, 0);
    ASSERT(near_rgba(px(4, 4), 0x7F4080BFu, 4));
    // destination minus source
    wl_gx_direct_blend(WL_GX_BM_SUBTRACT, WL_GX_BL_ONE, WL_GX_BL_ONE);
    quad(0, 0, 16, 16, 0.5f, 0x40000000u, 0, 0);
    ASSERT(near_rgba(px(4, 4), 0x3F4080BFu, 4));
    // the colour updates: alpha only
    wl_gx_direct_blend(WL_GX_BM_NONE, WL_GX_BL_ONE, WL_GX_BL_ZERO);
    wl_gx_direct_color_update(false, true);
    quad(0, 0, 16, 16, 0.5f, 0xFFFFFF00u, 0, 0);
    ASSERT(near_rgba(px(4, 4), 0x3F408000u, 4));
    end();
    // RGB8: eight bits, the alpha reads 255
    begin(0x000000FFu, false);
    layout(1, 0);
    quad(0, 0, 16, 16, 0.5f, 0x80402010u, 0, 0);
    ASSERT_EQ(0x804020FFu, px(4, 4));
    end();
    PASS();
}

// the EFB's format chosen before the software GX attaches (as the play harness's screenshots attach
// it mid-run): kept, so a destination alpha reads what was written, not 255 (a light added by
// DSTALPHA came out at full strength, the a10 tunnel washed out under the flashlight)
TEST the_efb_format_set_before_attaching_is_kept(void) {
    WlGxSoftConfig cfg;

    wl_gxsoft_detach();
    wl_gx_set_efb_alpha(true);
    memset(&cfg, 0, sizeof cfg);
    cfg.frame = on_frame;
    cfg.draw = on_draw;
    wl_gx_init();
    wl_host_gx_direct_reset();
    ASSERT_EQ(WL_OK, wl_gxsoft_attach(&cfg));
    wl_gx_begin_frame(0x00000000u);
    // (the clear's alpha 0 kept by RGBA6; RGB8 would read 255)
    ASSERT_EQ(0u, px(4, 4) & 0xFFu);
    wl_gx_end_frame();
    wl_gxsoft_detach();
    wl_gx_set_efb_alpha(false);
    wl_gx_shutdown();
    PASS();
}

TEST texture_sampling(void) {
    static const uint32_t checker[16] = {
        0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFFFFFFu, 0x000000FFu, 0x808080FFu, 0xFF00FFFFu, 0x00FFFFFFu,
        0x400000FFu, 0x004000FFu, 0x000040FFu, 0x404040FFu, 0x800000FFu, 0x008000FFu, 0x000080FFu, 0x202020FFu};
    static const uint32_t level1[4] = {0x102030FFu, 0x102030FFu, 0x102030FFu, 0x102030FFu};
    WlGxTexObj tex;
    void *image;

    begin(0x000000FFu, false);
    layout(1, 1);
    texture_replace();
    // nearest: a 4 x 4 texture over 64 x 64 pixels, 16 pixels a texel
    image = texture(&tex, WL_GX_TEX_RGBA8, 4, 4, checker, NULL, WL_GX_WRAP_CLAMP, WL_GX_FILTER_NEAREST,
                    WL_GX_FILTER_NEAREST);
    wl_gx_direct_texture(0, &tex, NULL, NULL);
    quad(0, 0, 64, 64, 0.5f, 0xFFFFFFFFu, 1, 1);
    ASSERT_EQ(0xFF0000FFu, px(2, 2));
    ASSERT_EQ(0x00FF00FFu, px(20, 2));
    ASSERT_EQ(0x202020FFu, px(60, 60));
    // repeat: twice over
    free(image);
    image = texture(&tex, WL_GX_TEX_RGBA8, 4, 4, checker, NULL, WL_GX_WRAP_REPEAT, WL_GX_FILTER_NEAREST,
                    WL_GX_FILTER_NEAREST);
    wl_gx_direct_texture(0, &tex, NULL, NULL);
    quad(100, 0, 164, 64, 0.5f, 0xFFFFFFFFu, 2, 2);
    ASSERT_EQ(0xFF0000FFu, px(132 + 1, 1));
    // bilinear: halfway between two texels' centres
    free(image);
    image = texture(&tex, WL_GX_TEX_RGBA8, 4, 4, checker, NULL, WL_GX_WRAP_CLAMP, WL_GX_FILTER_LINEAR,
                    WL_GX_FILTER_LINEAR);
    wl_gx_direct_texture(0, &tex, NULL, NULL);
    {
        // one pixel whose sample point is at s = 1/4 (between texels 0 and 1 of row 0's centre)
        WlGxDirectVertex v[4];
        float s0 = 0.25f - 7.0f / 12.0f / 640.0f;

        v[0] = vtx(200, 200, -0.5f, 0xFFFFFFFFu, s0, 0.125f);
        v[1] = vtx(840, 200, -0.5f, 0xFFFFFFFFu, s0 + 1.0f, 0.125f);
        v[2] = vtx(840, 201, -0.5f, 0xFFFFFFFFu, s0 + 1.0f, 0.125f);
        v[3] = vtx(200, 201, -0.5f, 0xFFFFFFFFu, s0, 0.125f);
        wl_gx_direct_draw(WL_GX_DIRECT_QUADS, v, 4);
        ASSERT(near_rgba(px(200, 200), 0x808000FFu, 2));
    }
    // levels: shrunk to a quarter, the second level (LINEAR_MIP_NEAREST)
    free(image);
    image = texture(&tex, WL_GX_TEX_RGBA8, 4, 4, checker, level1, WL_GX_WRAP_CLAMP, WL_GX_FILTER_NEAREST_MIP_NEAREST,
                    WL_GX_FILTER_NEAREST);
    wl_gx_direct_texture(0, &tex, NULL, NULL);
    quad(300, 0, 302, 2, 0.5f, 0xFFFFFFFFu, 1, 1);
    ASSERT_EQ(0x102030FFu, px(300, 0));
    // ...and magnified, the first
    quad(300, 100, 364, 164, 0.5f, 0xFFFFFFFFu, 1, 1);
    ASSERT_EQ(0xFF0000FFu, px(301, 101));
    free(image);
    end();
    PASS();
}

TEST cmpr_and_one_map_at_two_coordinates(void) {
    uint32_t texels[64];
    WlGxTexObj tex;
    WlTevProgram tev;
    WlTevStage *s;
    void *image;
    unsigned i;

    begin(0x000000FFu, false);
    // an 8 x 8 CMPR texture: left half red, right half blue (each sub-block one colour: exact)
    for (i = 0; i < 64; i++) {
        texels[i] = (i % 8u) < 4u ? 0xFF0000FFu : 0x0000FFFFu;
    }
    image = texture(&tex, WL_GX_TEX_CMPR, 8, 8, texels, NULL, WL_GX_WRAP_CLAMP, WL_GX_FILTER_NEAREST,
                    WL_GX_FILTER_NEAREST);
    wl_gx_direct_texture(0, &tex, NULL, NULL);
    layout(1, 3);
    // stage 0 reads map 0 at coordinate 0 into REG0, stage 1 the same map at coordinate 1 (s halved),
    // the result their sum
    wl_tev_program_init(&tev);
    s = wl_tev_add_stage(&tev);
    wl_tev_stage_order(s, 0, 0, WL_TEV_NONE);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_REG0);
    s = wl_tev_add_stage(&tev);
    wl_tev_stage_order(s, 1, 0, WL_TEV_NONE);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_TEXC, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    s = wl_tev_add_stage(&tev);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_CPREV, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    s->color.in[1] = WL_TEV_CC_C0;
    s->color.in[2] = WL_TEV_CC_ONE;
    wl_tev_stage_alpha(s, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_ZERO, WL_TEV_CA_KONST, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);
    wl_tev_apply_unchecked(&tev);
    quad(0, 0, 64, 64, 0.5f, 0xFFFFFFFFu, 1, 1);
    // on the right: coordinate 0 reads blue, coordinate 1 (s halved) red: their sum
    ASSERT_EQ_FMT(0xFF00FFFFu, px(56, 8), "%08x");
    // on the left both read red
    ASSERT_EQ(0xFF0000FFu, px(8, 8));
    free(image);
    end();
    PASS();
}

TEST render_target_copies_and_the_back_buffer_put_back(void) {
    WlGxTarget target;
    WlGxTexObj tex;
    WlGxTexDesc d;
    void *memory = aligned_alloc(32, wl_gx_tex_bytes(WL_GX_TEX_RGBA8, 64, 64));

    begin(0x000000FFu, false);
    layout(1, 0);
    // the back buffer drawn: green at its corner
    wl_gx_target_drawn();
    quad(0, 0, 100, 100, 0.5f, 0x00FF00FFu, 0, 0);
    // a target of 64 x 64 takes the EFB: red over part of it
    memset(&target, 0, sizeof target);
    target.id = &target;
    target.memory = memory;
    target.width = target.height = 64;
    target.format = WL_GX_TEX_RGBA8;
    wl_gx_target_use(&target);
    wl_gx_target_clear(true, true, 0x000000FFu, 1.0f, 0, 0, 64, 64);
    wl_gx_direct_begin();
    layout(1, 0);
    wl_gx_direct_viewport(0, 0, 640, 480);
    wl_gx_direct_blend(WL_GX_BM_NONE, WL_GX_BL_ONE, WL_GX_BL_ZERO);
    wl_gx_direct_zmode(false, WL_GX_ALWAYS, false);
    wl_gx_direct_alpha_compare(WL_GX_ALWAYS, 0);
    wl_gx_direct_color_update(true, true);
    {
        WlTevProgram tev;

        wl_tev_recipe(&tev, WL_GX_RECIPE_VERTEX_COLOR);
        wl_tev_apply_unchecked(&tev);
    }
    quad(0, 0, 32, 64, 0.5f, 0xFF0000FFu, 0, 0);
    ASSERT_EQ(0xFF0000FFu, px(10, 10));
    // the back buffer comes back: green again, the target resolved into its memory
    ASSERT(wl_gx_target_use(NULL) != NULL);
    ASSERT_EQ(0x00FF00FFu, px(10, 10));
    ASSERT_EQ(0x00FF00FFu, px(90, 90));
    // the target's image as a texture over the back buffer's middle
    memset(&d, 0, sizeof d);
    d.image = memory;
    d.width = d.height = 64;
    d.format = WL_GX_TEX_RGBA8;
    d.mips = 1;
    wl_gx_tex_init_ex(&tex, &d);
    wl_gx_direct_texture(0, &tex, NULL, NULL);
    layout(1, 1);
    texture_replace();
    quad(200, 200, 264, 264, 0.5f, 0xFFFFFFFFu, 1, 1);
    ASSERT_EQ(0xFF0000FFu, px(210, 230));
    ASSERT_EQ(0x000000FFu, px(250, 230));
    // the display copy
    wl_gx_end_frame();
    ASSERT_EQ(1u, g_frames);
    ASSERT_EQ(640u, g_frame_w);
    ASSERT_EQ(0xFFu, g_frame[(230u * 640u + 210u) * 4u]);
    free(memory);
    end();
    PASS();
}

TEST what_gx_takes_as_it_is_is_flagged(void) {
    WlTevProgram tev;

    begin(0x000000FFu, false);
    // a stage reading colour 1 and texture map 3 at coordinate 2, with one colour sent, no
    // coordinates and nothing in map 3
    layout(1, 0);
    wl_tev_recipe(&tev, WL_GX_RECIPE_TEX_MODULATE);
    tev.stages[0].channel = WL_TEV_CH_COLOR1A1;
    tev.stages[0].texcoord = 2;
    tev.stages[0].texmap = 3;
    wl_tev_apply_unchecked(&tev);
    quad(0, 0, 8, 8, 0.5f, 0xFFFFFFFFu, 0, 0);
    ASSERT(g_last.issues & WL_GXSOFT_ISSUE_NO_CHANNEL);
    ASSERT(g_last.issues & WL_GXSOFT_ISSUE_NO_TEXCOORD);
    ASSERT(g_last.issues & WL_GXSOFT_ISSUE_NO_TEXTURE);
    // ...which read 0 (the alpha 255: RGB8 has none)
    ASSERT_EQ_FMT(0x000000FFu, px(4, 4), "%08x");
    end();
    PASS();
}

SUITE(gxsoft_suite) {
    RUN_TEST(the_fill_rule_at_gx_sample_point);
    RUN_TEST(culling_by_the_screen_winding);
    RUN_TEST(the_near_plane_clips);
    RUN_TEST(the_depth_test_and_its_place);
    RUN_TEST(blending_and_the_efb_formats);
    RUN_TEST(the_efb_format_set_before_attaching_is_kept);
    RUN_TEST(texture_sampling);
    RUN_TEST(cmpr_and_one_map_at_two_coordinates);
    RUN_TEST(render_target_copies_and_the_back_buffer_put_back);
    RUN_TEST(what_gx_takes_as_it_is_is_flagged);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gxsoft_suite);
    GREATEST_MAIN_END();
}
