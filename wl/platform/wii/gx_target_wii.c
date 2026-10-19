// SPDX-License-Identifier: MIT
// The GPU's half of the render targets (gx_target.h): copies out of the EFB, and the quads that put
// the back buffer back and clear. Raw GX, in the order the calls matter; every function that
// leaves state behind calls wl_gx_invalidate_shadow.
#include <ogc/gu.h>
#include <ogc/gx.h>
#include <string.h>

#include "wl/platform/gx_direct.h"
#include "wl/platform/gx_target_internal.h"
#include "wl/platform/wii/gx_wii_internal.h"

void wl_gx_target_be_default_size(uint32_t *w, uint32_t *h) {
    WlVideoInfo vi;
    wl_platform_video_info(&vi);
    *w = vi.fb_width;
    *h = vi.efb_height;
}

void wl_gx_target_be_copy_begin(void) {
    wl_gx_wii_copy_filter(false);
}

void wl_gx_target_be_copy(void *dst, uint32_t x, uint32_t w, uint32_t h, WlGxTargetCopy what, bool invalidate) {
    // (the depth copy is a texture of RGBA8's size)
    static const u8 format[] = {GX_TF_RGBA8, GX_TF_RGB565, GX_TF_Z24X8};
    static const WlGxTexFormat size_of[] = {WL_GX_TEX_RGBA8, WL_GX_TEX_RGB565, WL_GX_TEX_RGBA8};
    if (wl_gx_bisect()->no_copies) {
        return; // (the bisection: the destination keeps what it held)
    }
    if (invalidate) {
        wl_gpu_buffer_invalidate(dst, wl_gx_tex_bytes(size_of[what], w, h));
    }
    GX_SetTexCopySrc((u16)x, 0, (u16)w, (u16)h);
    GX_SetTexCopyDst((u16)w, (u16)h, format[what], GX_FALSE);
    GX_CopyTex(dst, GX_FALSE);
    GX_PixModeSync();
    wl_gx_wii_hang_event('C', (uint32_t)(uintptr_t)dst, w << 16 | h);
    wl_gx_wii_note_efb_copy();
}

void wl_gx_target_be_copy_end(void) {
    wl_gx_wii_copy_filter(true);
    // (GX's texture cache may hold what was in the destinations)
    GX_InvalidateTexAll();
    wl_gx_wii_note_commands();
}

// (gx_wii.c's; weak: this file's test links its own wl_gx_invalidate_shadow alone)
void wl_gx_wii_invalidate_shadow_own(void) __attribute__((weak));

// the shadow forgotten after the GX set here, the maps this file loaded forgotten already (wl_gx_wii_texmaps_loaded)
static void invalidate_own(void) {
    if (wl_gx_wii_invalidate_shadow_own != NULL) {
        wl_gx_wii_invalidate_shadow_own();
    } else {
        wl_gx_invalidate_shadow();
    }
}

void wl_gx_target_be_scissor(uint32_t x, uint32_t w, uint32_t h) {
    GX_SetScissor(x, 0, w, h);
    invalidate_own();
}

// A quad over a rectangle of the back buffer's pixels: its colour and alpha from texture map 0 or a
// constant colour, its depth from texture map 1 (a Z texture, when `z_texture`) or a constant depth.
static void quad(float x0, float y0, float x1, float y1, const GXColor *colour, float depth, bool textured,
                 bool z_texture, uint32_t back_w, uint32_t back_h) {
    Mtx44 matrix;
    Mtx identity;
    static const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    if (wl_gx_bisect()->no_quads) {
        invalidate_own(); // (the bisection: no quad; the state its callers set stays)
        return;
    }
    memset(matrix, 0, sizeof matrix);
    matrix[0][0] = 2.0f / (float)back_w;
    matrix[0][3] = -1.0f;
    matrix[1][1] = -2.0f / (float)back_h;
    matrix[1][3] = 1.0f;
    matrix[2][2] = 1.0f;
    matrix[3][3] = 1.0f;
    GX_LoadProjectionMtx(matrix, GX_ORTHOGRAPHIC);
    GX_SetViewport(0.0f, 0.0f, (f32)back_w, (f32)back_h, 0.0f, 1.0f);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetCullMode(GX_CULL_NONE);
    // (no stage reads an indirect texture)
    GX_SetNumIndStages(0);
    GX_SetTevDirect(GX_TEVSTAGE0);
    GX_SetTevDirect(GX_TEVSTAGE1);
    // (no stage reads a channel; an untextured quad still has one, from the registers: GX never has
    // no channel and no coordinate at once, gx_direct_wii.c)
    GX_SetNumChans(textured ? 0 : 1);
    if (!textured) {
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    }
    GX_SetNumTevStages(textured && z_texture ? 2 : 1);
    if (textured) {
        GX_SetNumTexGens(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
        if (z_texture) {
            // (the Z texture is the last stage's texture; the colour passes)
            GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
            GX_SetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
            GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
            GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0);
            GX_SetZCompLoc(GX_FALSE);
        }
    } else {
        GX_SetNumTexGens(0);
        GX_SetTevColor(GX_TEVREG0, *colour);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    // the vertices: positions in pixels (the depth as z - 1, GX's clip-space depth running from -1,
    // near, to 0, far), and the texture coordinate when a texture is read
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    if (textured) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    }

    wl_gx_wii_hang_event('Q', (textured ? 1u : 0u) | (z_texture ? 2u : 0u), back_w << 16 | back_h);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (int corner = 0; corner < 4; corner++) {
        GX_Position3f32(x0 + (x1 - x0) * corners[corner][0], y0 + (y1 - y0) * corners[corner][1], depth - 1.0f);
        if (textured) {
            GX_TexCoord2f32(corners[corner][0], corners[corner][1]);
        }
    }
    GX_End();
    if (textured && z_texture) {
        GX_SetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
        GX_SetZCompLoc(GX_TRUE);
    }
    invalidate_own();
    wl_gx_wii_note_commands();
}

void wl_gx_target_be_restore(const void *colour, const void *depth, uint32_t x, uint32_t w, uint32_t h,
                             uint32_t back_w, uint32_t back_h) {
    GXTexObj colour_map, depth_map;

    GX_InvalidateTexAll();
    GX_InitTexObj(&colour_map, (void *)colour, (u16)w, (u16)h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&colour_map, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&colour_map, GX_TEXMAP0);
    wl_gx_wii_texmaps_loaded(1u << GX_TEXMAP0);
    if (depth == NULL || wl_gx_bisect()->no_z_restore) {
        // (the colour alone: the depth there is the back buffer's still)
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_TRUE);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GX_SetZCompLoc(GX_TRUE);
        quad((float)x, 0.0f, (float)(x + w), (float)h, NULL, 0.0f, true, false, back_w, back_h);
        return;
    }
    GX_InitTexObj(&depth_map, (void *)depth, (u16)w, (u16)h, GX_TF_Z24X8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&depth_map, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&depth_map, GX_TEXMAP1);
    wl_gx_wii_texmaps_loaded(1u << GX_TEXMAP1);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    quad((float)x, 0.0f, (float)(x + w), (float)h, NULL, 0.0f, true, true, back_w, back_h);
}

void wl_gx_target_be_clear(bool colour, bool depth, uint32_t rgba, float z, int x, int y, int rw, int rh,
                           uint32_t back_w, uint32_t back_h) {
    GXColor c = {(u8)(rgba >> 24), (u8)(rgba >> 16), (u8)(rgba >> 8), (u8)rgba};
    float x0 = (float)x, y0 = (float)y;

    GX_SetColorUpdate(colour ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate(colour ? GX_TRUE : GX_FALSE);
    GX_SetZMode(depth ? GX_TRUE : GX_FALSE, GX_ALWAYS, depth ? GX_TRUE : GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    quad(x0, y0, x0 + (float)rw, y0 + (float)rh, &c, z, false, false, back_w, back_h);
}

void wl_gx_target_be_squeeze(void *colour, uint32_t back_w, uint32_t back_h, uint32_t x, uint32_t w,
                             bool invalidate) {
    GXTexObj map;

    // The finished picture out of the EFB, which the same copy clears to black for the bars. The
    // clear writes colour, alpha and depth only where their updates are on.
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetCopyClear((GXColor){0, 0, 0, 255}, 0x00FFFFFFu);
    wl_gx_wii_copy_filter(false);
    if (invalidate) {
        wl_gpu_buffer_invalidate(colour, wl_gx_tex_bytes(WL_GX_TEX_RGBA8, back_w, back_h));
    }
    GX_SetTexCopySrc(0, 0, (u16)back_w, (u16)back_h);
    GX_SetTexCopyDst((u16)back_w, (u16)back_h, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(colour, GX_TRUE);
    GX_PixModeSync();
    wl_gx_wii_hang_event('C', (uint32_t)(uintptr_t)colour, back_w << 16 | back_h);
    wl_gx_wii_note_efb_copy();
    wl_gx_wii_copy_filter(true);
    GX_InvalidateTexAll();
    // ...and drawn back narrower, filtered, over the bars
    GX_InitTexObj(&map, colour, (u16)back_w, (u16)back_h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&map, GX_LINEAR, GX_LINEAR);
    GX_LoadTexObj(&map, GX_TEXMAP0);
    wl_gx_wii_texmaps_loaded(1u << GX_TEXMAP0);
    GX_SetScissor(0, 0, (u32)back_w, (u32)back_h);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    quad((float)x, 0.0f, (float)(x + w), (float)back_h, NULL, 0.0f, true, false, back_w, back_h);
}
