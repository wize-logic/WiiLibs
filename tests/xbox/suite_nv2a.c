// SPDX-License-Identifier: MIT
// The NV2A backend on the Wii's GX (wl/xbox/nv2a_gx.h), for the on-console runner: synthetic push
// buffers (tests/xbox/nv2a_frames.h) run through the device model into GX, and the EFB read back
// before each frame is shown. A textured quad through a combiner program, two combiner stages with
// their own constants, a triangle through a vertex program that divides by w (GX's perspective
// projection), the depth test, alpha blending. Each check allows 8 a channel (the EFB keeps 6 bits
// a channel with a destination alpha).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tests/platform/check.h"
#include "tests/xbox/nv2a_test_util.h"
#include "tests/xbox/nv2a_frames.h"
#include "wl/platform/platform.h"
#include "wl/xbox/nv2a_gx.h"

void wlt_suite_nv2a(void);

static bool near8(uint32_t got, uint32_t want) {
    int shift;

    for (shift = 8; shift < 32; shift += 8) {
        int d = (int)((got >> shift) & 0xFFu) - (int)((want >> shift) & 0xFFu);

        if (d > 8 || d < -8) {
            return false;
        }
    }
    return true;
}

static void expect_pixel(const char *name, int x, int y, uint32_t want) {
    char detail[96];
    uint32_t got = wl_gx_peek_rgba(x, y);

    snprintf(detail, sizeof detail, "(%d, %d) 0x%08x, want 0x%08x", x, y, (unsigned)got, (unsigned)want);
    wlt_check(name, near8(got, want), detail);
}

static void white_texture(TGuest *g) {
    unsigned i;

    for (i = 0; i < 16; i++) {
        t_w32(g, F_TEXTURE + 4 * i, 0xFFFFFFFFu);
    }
}

void wlt_suite_nv2a(void) {
    TGuest g;
    WlNv2aGx *gx = wl_nv2a_gx_create(NULL);
    WlNv2aBackend backend;
    WlNv2aGxStats stats;
    WlNv2aStats model;

    wlt_report("WLT SUITE nv2a");
    wlt_check("nv2a_gx_create", gx != NULL, "wl_nv2a_gx_create");
    if (gx == NULL) {
        return;
    }
    wl_nv2a_gx_backend(gx, &backend);
    wlt_check("nv2a_create", t_create(&g, &backend) != NULL, "a device over 4 MiB of guest RAM");
    if (g.dev == NULL) {
        wl_nv2a_gx_destroy(gx);
        return;
    }
    wl_nv2a_gx_attach(gx, g.dev);
    t_setup_3d(&g);
    f_frame_setup(&g);

    // 1: the clear, a grey texture times white over (100, 100)-(200, 200), times red beside it
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF204080u);
    f_quad(&g, 100, 100, 200, 200, 0.5f, 0xFFFFFFFFu);
    f_quad(&g, 300, 100, 400, 200, 0.5f, 0xFF0000FFu);
    t_kick(&g);
    expect_pixel("nv2a_clear", 50, 50, 0x204080FFu);
    expect_pixel("nv2a_textured_quad", 150, 150, 0x808080FFu);
    expect_pixel("nv2a_textured_quad_edge", 199, 199, 0x808080FFu);
    expect_pixel("nv2a_textured_quad_outside", 201, 150, 0x204080FFu);
    expect_pixel("nv2a_texture_times_colour", 350, 150, 0x800000FFu);
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(&g);

    // 2: two combiner stages, r1 = t0 * c0 + v0 * c1: white * (255, 128, 0) + grey * (0, 0, 128)
    white_texture(&g);
    f_clear(&g, 0xFF000000u);
    f_combiner_two_stages(&g, 0xFFFF8000u, 0xFF000080u);
    f_quad(&g, 0, 0, 64, 64, 0.5f, 0xFF808080u);
    t_kick(&g);
    expect_pixel("nv2a_two_combiner_stages", 32, 32, 0xFF8040FFu);
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(&g);

    // 3: a green triangle through the vertex program that divides by w: (0,0,2) the screen's
    // middle, (1,1,4) at (400,180), (-1,1,4) at (240,180)
    f_combiner_texture_times_colour(&g);
    f_clear(&g, 0xFF000000u);
    f_perspective(&g);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, WL_NV2A_PRIM_TRIANGLES);
    f_vertex(&g, 0, 0, 2, 1, 0, 0, 0xFF00FF00u);
    f_vertex(&g, 1, 1, 4, 1, 1, 0, 0xFF00FF00u);
    f_vertex(&g, -1, 1, 4, 1, 0, 1, 0xFF00FF00u);
    t_m1(&g, WL_NV2A_K_SET_BEGIN_END, 0);
    t_kick(&g);
    expect_pixel("nv2a_vertex_program_inside", 320, 200, 0x00FF00FFu);
    expect_pixel("nv2a_vertex_program_outside", 320, 260, 0x000000FFu);
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(&g);

    // 4: the depth test (red at 0.3, green behind it at 0.7, blue in front at 0.1) and blending
    // (half-transparent blue over red)
    f_program(&g, 0, f_screen_program, 3);
    f_clear(&g, 0xFF000000u);
    f_quad(&g, 100, 100, 200, 200, 0.3f, 0xFF0000FFu);
    f_quad(&g, 150, 150, 250, 250, 0.7f, 0xFF00FF00u);
    f_quad(&g, 180, 180, 220, 220, 0.1f, 0xFFFF0000u);
    f_quad(&g, 400, 100, 464, 164, 0.5f, 0xFF0000FFu);
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 1);
    t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_SFACTOR, WL_NV2A_BLEND_SRC_ALPHA);
    t_m1(&g, WL_NV2A_K_SET_BLEND_FUNC_DFACTOR, WL_NV2A_BLEND_ONE_MINUS_SRC_ALPHA);
    t_m1(&g, WL_NV2A_K_SET_BLEND_EQUATION, WL_NV2A_BLEND_EQ_ADD);
    t_m1(&g, WL_NV2A_K_SET_DEPTH_TEST_ENABLE, 0);
    f_quad(&g, 400, 100, 464, 164, 0.5f, 0x80FF0000u);
    t_m1(&g, WL_NV2A_K_SET_BLEND_ENABLE, 0);
    t_m1(&g, WL_NV2A_K_SET_DEPTH_TEST_ENABLE, 1);
    t_kick(&g);
    expect_pixel("nv2a_depth_nearer_kept", 160, 160, 0xFF0000FFu);
    expect_pixel("nv2a_depth_where_nothing_was", 240, 240, 0x00FF00FFu);
    expect_pixel("nv2a_depth_in_front", 190, 190, 0x0000FFFFu);
    expect_pixel("nv2a_alpha_blend", 432, 132, 0x7F0080FFu);
    t_m1(&g, WL_NV2A_K_FLIP_INCREMENT_WRITE, 0);
    t_kick(&g);

    wl_nv2a_gx_stats(gx, &stats);
    wl_nv2a_stats(g.dev, &model);
    wlt_report("INFO nv2a: %u draws, %u vertices, %u clears (%u quads), %u frames, %u perspective draws, %u "
               "programs beyond GX, %u approximated; model: %u methods, %u not handled, %u pusher errors",
               (unsigned)stats.draws, (unsigned)stats.vertices, (unsigned)stats.clears, (unsigned)stats.quad_clears,
               (unsigned)stats.frames, (unsigned)stats.perspective_draws, (unsigned)stats.programs_unusable,
               (unsigned)stats.approximations, (unsigned)model.methods, (unsigned)model.unhandled,
               (unsigned)model.pusher_errors);
    wlt_check("nv2a_frames_shown", stats.frames == 4u, "four frames shown on FLIP_INCREMENT_WRITE");
    wlt_check("nv2a_no_pusher_errors", model.pusher_errors == 0u, "the push buffers decoded cleanly");
    wlt_check("nv2a_programs_on_gx", stats.programs_unusable == 0u, "every combiner program fit the TEV");
    {
        WlGxStats gs;

        wl_gx_stats(&gs);
        wlt_check("nv2a_no_gx_timeouts", gs.gpu_timeouts == 0u, "the GPU finished every frame");
    }
    wl_nv2a_gx_destroy(gx);
    t_destroy(&g);
    wl_gx_invalidate_shadow();
}
