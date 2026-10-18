// SPDX-License-Identifier: MIT
// The shadow of a texture map's registers and of the vertex arrays (wl/platform/gx_shadow.c): what a
// bind writes (nothing, the registers that differ, the whole load), the switches for the old ways,
// forgetting, and the counts.
#include "wl/platform/gx_shadow.h"

#include <string.h>

#include "greatest.h"

static void regs_of(uint32_t regs[WL_GX_TEXREGS], uintptr_t address, uint32_t width, uint32_t wrap) {
    wl_gx_shadow_texture_model(regs, address, WL_GX_TEX_CMPR, width, 256, 9, wrap, wrap, WL_GX_FILTER_LINEAR_MIP_LINEAR,
                               WL_GX_FILTER_LINEAR, 0.0f, 0, wl_gx_shadow_texture_region(WL_GX_TEX_CMPR, 9, false));
}

TEST texture_binds_write_what_differs(void) {
    uint32_t a[WL_GX_TEXREGS], b[WL_GX_TEXREGS], c[WL_GX_TEXREGS], binds, changed, loads, writes;

    wl_gx_shadow_texture_forget(0xFFu);
    regs_of(a, 0x10000, 256, WL_GX_WRAP_REPEAT);
    regs_of(b, 0x20000, 256, WL_GX_WRAP_REPEAT); // (another address: image 3 alone)
    regs_of(c, 0x30000, 128, WL_GX_WRAP_REPEAT); // (another size: the whole load)
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(2, a, 6, false));
    ASSERT_EQ(0u, wl_gx_shadow_texture(2, a, 6, false));
    ASSERT_EQ(1u << WL_GX_TEXREG_IMAGE3, wl_gx_shadow_texture(2, b, 6, false));
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(2, c, 6, false));
    regs_of(c, 0x30000, 128, WL_GX_WRAP_CLAMP); // (another mode 0: the whole load)
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(2, c, 6, false));
    // (another map knows nothing yet; the switch gxtexload loads whole at every bind)
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(3, c, 6, false));
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(3, c, 6, true));
    // (forgotten: loaded whole again)
    wl_gx_shadow_texture_forget(1u << 2);
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(2, c, 6, false));
    wl_gx_shadow_texture_kept();
    wl_gx_shadow_texture_counts(&binds, &changed, &loads, &writes);
    ASSERT(binds >= 9u && changed + 2u <= binds && loads + 1u <= changed);
    PASS();
}

TEST texture_tlut_register(void) {
    uint32_t a[WL_GX_TEXREGS], b[WL_GX_TEXREGS];

    wl_gx_shadow_texture_forget(0xFFu);
    wl_gx_shadow_texture_model(a, 0x10000, WL_GX_TEX_C8, 64, 64, 1, 0, 0, 1, 1, 0.0f, 3,
                               wl_gx_shadow_texture_region(WL_GX_TEX_C8, 1, false));
    memcpy(b, a, sizeof b);
    b[WL_GX_TEXREG_TLUT] = 4;
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x7Fu, wl_gx_shadow_texture(0, a, 7, false));
    ASSERT_EQ(1u << WL_GX_TEXREG_TLUT, wl_gx_shadow_texture(0, b, 7, false));
    // (a texture without a TLUT after one with: the whole load)
    ASSERT_EQ(WL_GX_TEXREG_LOAD | 0x3Fu, wl_gx_shadow_texture(0, b, 6, false));
    PASS();
}

TEST texture_regions(void) {
    // (the even bank alone for colour-indexed textures and CMPR without mipmaps; libogc's three sets)
    ASSERT_EQ(1u, wl_gx_shadow_texture_region(WL_GX_TEX_C4, 1, false));
    ASSERT_EQ(1u, wl_gx_shadow_texture_region(WL_GX_TEX_CMPR, 1, false));
    ASSERT_EQ(0u, wl_gx_shadow_texture_region(WL_GX_TEX_CMPR, 4, false));
    ASSERT_EQ(0u, wl_gx_shadow_texture_region(WL_GX_TEX_RGBA8, 1, false));
    ASSERT_EQ(0u, wl_gx_shadow_texture_region(WL_GX_TEX_C8, 1, true));
    ASSERT_EQ(2u, wl_gx_shadow_texture_region(WL_GX_TEX_RGBA8, 3, true));
    ASSERT_EQ(1u, wl_gx_shadow_texture_region(WL_GX_TEX_RGBA8, 1, true));
    PASS();
}

TEST arrays_sent_when_they_differ(void) {
    static uint8_t data[64];
    uint32_t calls, sent, calls0, sent0;

    wl_gx_shadow_forget();
    wl_gx_shadow_array_counts(&calls0, &sent0);
    ASSERT(wl_gx_shadow_array(0, data, 12, false));
    ASSERT_FALSE(wl_gx_shadow_array(0, data, 12, false));
    ASSERT(wl_gx_shadow_array(0, data, 16, false));
    ASSERT(wl_gx_shadow_array(0, data + 16, 16, false));
    ASSERT(wl_gx_shadow_array(0, data + 16, 16, true)); // (gxarrayall)
    wl_gx_shadow_array_forget();
    ASSERT(wl_gx_shadow_array(0, data + 16, 16, false));
    wl_gx_shadow_forget();
    ASSERT(wl_gx_shadow_array(0, data + 16, 16, false));
    wl_gx_shadow_array_counts(&calls, &sent);
    ASSERT_EQ(7u, calls - calls0);
    ASSERT_EQ(6u, sent - sent0);
    PASS();
}

SUITE(gx_shadow_suite) {
    RUN_TEST(texture_binds_write_what_differs);
    RUN_TEST(texture_tlut_register);
    RUN_TEST(texture_regions);
    RUN_TEST(arrays_sent_when_they_differ);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(gx_shadow_suite);
    GREATEST_MAIN_END();
}
