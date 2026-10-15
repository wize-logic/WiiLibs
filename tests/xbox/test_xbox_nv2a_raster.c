// SPDX-License-Identifier: MIT
// wl/xbox/nv2a_raster.c: the NV2A's raster state as GX takes it, from the values alone. Linked by
// itself (no other part of wl_xbox), as a Direct3D device's host harness links it (OpenCE-Wii's
// draw bench). The compare functions in GX's order, blending exact or not, Direct3D's cull modes
// and the NV2A's, and GX's cull mode either way round of the measured winding.
#include "greatest.h"
#include "wl/xbox/nv2a_pipeline.h"
#include "wl/xbox/nv2a_regs.h"

TEST compare_functions_in_gx_order(void) {
    static const WlGxCompare want[8] = {WL_GX_NEVER,   WL_GX_LESS,   WL_GX_EQUAL,  WL_GX_LEQUAL,
                                        WL_GX_GREATER, WL_GX_NEQUAL, WL_GX_GEQUAL, WL_GX_ALWAYS};
    unsigned i;

    for (i = 0; i < 8; i++) {
        ASSERT_EQ(want[i], wl_nv2a_gx_compare(WL_NV2A_FUNC_NEVER + i));
    }
    ASSERT_EQ(WL_GX_ALWAYS, wl_nv2a_gx_compare(0));
    ASSERT_EQ(WL_GX_ALWAYS, wl_nv2a_gx_compare(0x208));
    PASS();
}

TEST blending(void) {
    WlGxBlendMode mode;
    WlGxBlendFactor source, destination;

    // off: one and zero
    ASSERT(wl_nv2a_gx_blend_of(false, WL_NV2A_BLEND_EQ_ADD, WL_NV2A_BLEND_SRC_ALPHA,
                               WL_NV2A_BLEND_ONE, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BM_NONE, mode);
    ASSERT_EQ(WL_GX_BL_ONE, source);
    ASSERT_EQ(WL_GX_BL_ZERO, destination);
    // the usual alpha blend, exact
    ASSERT(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_ADD, WL_NV2A_BLEND_SRC_ALPHA,
                               WL_NV2A_BLEND_ONE_MINUS_SRC_ALPHA, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BM_BLEND, mode);
    ASSERT_EQ(WL_GX_BL_SRCALPHA, source);
    ASSERT_EQ(WL_GX_BL_INVSRCALPHA, destination);
    // a multiply (destination colour times source): GX's source factor of the other side's colour
    ASSERT(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_ADD, WL_NV2A_BLEND_DST_COLOR,
                               WL_NV2A_BLEND_ZERO, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BL_OTHERCLR, source);
    // the source's own colour as its factor: GX has none (one, not exact)
    ASSERT_FALSE(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_ADD, WL_NV2A_BLEND_SRC_COLOR,
                                     WL_NV2A_BLEND_ONE, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BL_ONE, source);
    // reverse subtract (signed too): GX's subtract, exact with factors of one
    ASSERT(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT, WL_NV2A_BLEND_ONE,
                               WL_NV2A_BLEND_ONE, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BM_SUBTRACT, mode);
    ASSERT(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_REVERSE_SUBTRACT_SIGNED, WL_NV2A_BLEND_ONE,
                               WL_NV2A_BLEND_ONE, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BM_SUBTRACT, mode);
    // MIN, MAX: blended, not exact; MIN darkens (s d), MAX brightens no more than s + d - s d
    ASSERT_FALSE(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_MAX, WL_NV2A_BLEND_ONE,
                                     WL_NV2A_BLEND_ONE, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BM_BLEND, mode);
    ASSERT_EQ(WL_GX_BL_INVOTHERCLR, source);
    ASSERT_EQ(WL_GX_BL_ONE, destination);
    ASSERT_FALSE(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_MIN, WL_NV2A_BLEND_ONE,
                                     WL_NV2A_BLEND_ONE, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BM_BLEND, mode);
    ASSERT_EQ(WL_GX_BL_OTHERCLR, source);
    ASSERT_EQ(WL_GX_BL_ZERO, destination);
    // s - d: s (1 - d), darker where the destination is bright; s f - 0 exact
    ASSERT_FALSE(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_SUBTRACT, WL_NV2A_BLEND_ONE,
                                     WL_NV2A_BLEND_ONE, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BM_BLEND, mode);
    ASSERT_EQ(WL_GX_BL_INVOTHERCLR, source);
    ASSERT_EQ(WL_GX_BL_ZERO, destination);
    ASSERT(wl_nv2a_gx_blend_of(true, WL_NV2A_BLEND_EQ_SUBTRACT, WL_NV2A_BLEND_SRC_ALPHA,
                               WL_NV2A_BLEND_ZERO, &mode, &source, &destination));
    ASSERT_EQ(WL_GX_BL_SRCALPHA, source);
    PASS();
}

TEST culling(void) {
    // Direct3D's modes: the faces of that winding on the screen (clockwise: a positive area, y
    // down)
    ASSERT_EQ(WL_NV2A_CULLS_NONE, wl_nv2a_cull_d3d(0));
    ASSERT_EQ(WL_NV2A_CULLS_POSITIVE, wl_nv2a_cull_d3d(WL_NV2A_FRONT_CW));
    ASSERT_EQ(WL_NV2A_CULLS_NEGATIVE, wl_nv2a_cull_d3d(WL_NV2A_FRONT_CCW));
    ASSERT_EQ(WL_NV2A_CULLS_NONE, wl_nv2a_cull_d3d(1));
    // the NV2A's: off, the back of a CW front (the negative), front and back
    ASSERT_EQ(WL_NV2A_CULLS_NONE, wl_nv2a_cull_of(false, WL_NV2A_CULL_BACK, WL_NV2A_FRONT_CW));
    ASSERT_EQ(WL_NV2A_CULLS_NEGATIVE, wl_nv2a_cull_of(true, WL_NV2A_CULL_BACK, WL_NV2A_FRONT_CW));
    ASSERT_EQ(WL_NV2A_CULLS_POSITIVE, wl_nv2a_cull_of(true, WL_NV2A_CULL_FRONT, WL_NV2A_FRONT_CW));
    ASSERT_EQ(WL_NV2A_CULLS_ALL,
              wl_nv2a_cull_of(true, WL_NV2A_CULL_FRONT_AND_BACK, WL_NV2A_FRONT_CCW));
    // GX's mode, whichever of its names culls the positive
    ASSERT_EQ(WL_GX_CULL_FRONT, wl_nv2a_gx_cull(WL_NV2A_CULLS_POSITIVE, WL_GX_CULL_FRONT));
    ASSERT_EQ(WL_GX_CULL_BACK, wl_nv2a_gx_cull(WL_NV2A_CULLS_NEGATIVE, WL_GX_CULL_FRONT));
    ASSERT_EQ(WL_GX_CULL_FRONT, wl_nv2a_gx_cull(WL_NV2A_CULLS_NEGATIVE, WL_GX_CULL_BACK));
    ASSERT_EQ(WL_GX_CULL_ALL, wl_nv2a_gx_cull(WL_NV2A_CULLS_ALL, WL_GX_CULL_BACK));
    ASSERT_EQ(WL_GX_CULL_NONE, wl_nv2a_gx_cull(WL_NV2A_CULLS_NONE, WL_GX_CULL_BACK));
    PASS();
}

SUITE(raster_suite) {
    RUN_TEST(compare_functions_in_gx_order);
    RUN_TEST(blending);
    RUN_TEST(culling);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(raster_suite);
    GREATEST_MAIN_END();
}
