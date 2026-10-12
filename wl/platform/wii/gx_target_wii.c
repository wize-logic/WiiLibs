// SPDX-License-Identifier: MIT
// The GPU's half of the render targets (gx_target.h): copies out of the EFB, and the quads that put
// the back buffer back and clear. Raw GX, in the order the calls matter; every function that
// leaves state behind calls wl_gx_invalidate_shadow.
#include <ogc/gu.h>
#include <ogc/gx.h>
#include <string.h>

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

void wl_gx_target_be_copy(void *dst, uint32_t w, uint32_t h, WlGxTargetCopy what) {
    // (the depth copy is a texture of RGBA8's size)
    static const u8 format[] = {GX_TF_RGBA8, GX_TF_RGB565, GX_TF_Z24X8};
    static const WlGxTexFormat size_of[] = {WL_GX_TEX_RGBA8, WL_GX_TEX_RGB565, WL_GX_TEX_RGBA8};
    wl_gpu_buffer_invalidate(dst, wl_gx_tex_bytes(size_of[what], w, h));
    GX_SetTexCopySrc(0, 0, (u16)w, (u16)h);
    GX_SetTexCopyDst((u16)w, (u16)h, format[what], GX_FALSE);
    GX_CopyTex(dst, GX_FALSE);
    GX_PixModeSync();
}

void wl_gx_target_be_copy_end(void) {
    wl_gx_wii_copy_filter(true);
    // (GX's texture cache may hold what was in the destinations)
    GX_InvalidateTexAll();
    wl_gx_wii_note_commands();
}

void wl_gx_target_be_scissor(uint32_t w, uint32_t h) {
    GX_SetScissor(0, 0, w, h);
    wl_gx_invalidate_shadow();
}

// A quad over a rectangle of the back buffer's pixels: its colour and alpha from texture map 0 or a
// constant colour, its depth from texture map 1 (a Z texture) or a constant depth.
static void quad(float x0, float y0, float x1, float y1, const GXColor *colour, float depth, bool textured,
                 uint32_t back_w, uint32_t back_h) {
    Mtx44 matrix;
    Mtx identity;
    static const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

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
    GX_SetNumChans(0);
    GX_SetNumTevStages(textured ? 2 : 1);
    if (textured) {
        GX_SetNumTexGens(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
        // (the Z texture is the last stage's texture; the colour passes)
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
        GX_SetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0);
        GX_SetZCompLoc(GX_FALSE);
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

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (int corner = 0; corner < 4; corner++) {
        GX_Position3f32(x0 + (x1 - x0) * corners[corner][0], y0 + (y1 - y0) * corners[corner][1], depth - 1.0f);
        if (textured) {
            GX_TexCoord2f32(corners[corner][0], corners[corner][1]);
        }
    }
    GX_End();
    if (textured) {
        GX_SetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
        GX_SetZCompLoc(GX_TRUE);
    }
    wl_gx_invalidate_shadow();
    wl_gx_wii_note_commands();
}

void wl_gx_target_be_restore(const void *colour, const void *depth, uint32_t w, uint32_t h) {
    GXTexObj colour_map, depth_map;

    GX_InvalidateTexAll();
    GX_InitTexObj(&colour_map, (void *)colour, (u16)w, (u16)h, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&colour_map, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&colour_map, GX_TEXMAP0);
    GX_InitTexObj(&depth_map, (void *)depth, (u16)w, (u16)h, GX_TF_Z24X8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&depth_map, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&depth_map, GX_TEXMAP1);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    quad(0.0f, 0.0f, (float)w, (float)h, NULL, 0.0f, true, w, h);
}

void wl_gx_target_be_clear(bool colour, bool depth, uint32_t rgba, float z, int x, int y, int rw, int rh,
                           uint32_t back_w, uint32_t back_h) {
    GXColor c = {(u8)(rgba >> 24), (u8)(rgba >> 16), (u8)(rgba >> 8), (u8)rgba};
    float x0 = (float)x, y0 = (float)y;

    GX_SetColorUpdate(colour ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate(colour ? GX_TRUE : GX_FALSE);
    GX_SetZMode(depth ? GX_TRUE : GX_FALSE, GX_ALWAYS, depth ? GX_TRUE : GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    quad(x0, y0, x0 + (float)rw, y0 + (float)rh, &c, z, false, back_w, back_h);
}
