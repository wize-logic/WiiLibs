// SPDX-License-Identifier: MIT
// A stand-in for the TEV part of libogc's <ogc/gx.h> (test only): the constants with libogc's
// values, and calls that record the state they would write, so wl/platform/wii/tev_wii.c
// can be compiled and run on the host. tev_wii.c's static asserts compare tev.h against these
// values; the Wii build compares them against the real header.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef struct {
    u8 r, g, b, a;
} GXColor;

#define GX_TRUE 1
#define GX_FALSE 0

#define GX_COLOR0A0 4
#define GX_COLOR1A1 5
#define GX_COLORZERO 6
#define GX_COLORNULL 0xff
#define GX_TEXCOORD0 0
#define GX_TEXCOORDNULL 0xff
#define GX_TEXMAP0 0
#define GX_MAX_TEXMAP 8
#define GX_TEXMAP_NULL 0xff

#define GX_CC_CPREV 0
#define GX_CC_APREV 1
#define GX_CC_C0 2
#define GX_CC_A0 3
#define GX_CC_C1 4
#define GX_CC_A1 5
#define GX_CC_C2 6
#define GX_CC_A2 7
#define GX_CC_TEXC 8
#define GX_CC_TEXA 9
#define GX_CC_RASC 10
#define GX_CC_RASA 11
#define GX_CC_ONE 12
#define GX_CC_HALF 13
#define GX_CC_KONST 14
#define GX_CC_ZERO 15
#define GX_CA_APREV 0
#define GX_CA_A0 1
#define GX_CA_A1 2
#define GX_CA_A2 3
#define GX_CA_TEXA 4
#define GX_CA_RASA 5
#define GX_CA_KONST 6
#define GX_CA_ZERO 7

#define GX_TEVSTAGE0 0
#define GX_MAX_TEVSTAGE 16
#define GX_TEV_ADD 0
#define GX_TEV_SUB 1
#define GX_TEV_COMP_R8_GT 8
#define GX_TEV_COMP_R8_EQ 9
#define GX_TEV_COMP_GR16_GT 10
#define GX_TEV_COMP_GR16_EQ 11
#define GX_TEV_COMP_BGR24_GT 12
#define GX_TEV_COMP_BGR24_EQ 13
#define GX_TEV_COMP_RGB8_GT 14
#define GX_TEV_COMP_RGB8_EQ 15
#define GX_TEV_COMP_A8_GT GX_TEV_COMP_RGB8_GT
#define GX_TEV_COMP_A8_EQ GX_TEV_COMP_RGB8_EQ
#define GX_TB_ZERO 0
#define GX_TB_ADDHALF 1
#define GX_TB_SUBHALF 2
#define GX_MAX_TEVBIAS 3
#define GX_CS_SCALE_1 0
#define GX_CS_SCALE_2 1
#define GX_CS_SCALE_4 2
#define GX_CS_DIVIDE_2 3
#define GX_MAX_TEVSCALE 4
#define GX_TEVPREV 0
#define GX_TEVREG0 1
#define GX_TEVREG1 2
#define GX_TEVREG2 3
#define GX_MAX_TEVREG 4

#define GX_KCOLOR0 0
#define GX_KCOLOR_MAX 4
#define GX_TEV_KCSEL_1 0x00
#define GX_TEV_KCSEL_1_2 0x04
#define GX_TEV_KCSEL_1_8 0x07
#define GX_TEV_KCSEL_K0 0x0C
#define GX_TEV_KCSEL_K0_R 0x10
#define GX_TEV_KCSEL_K0_G 0x14
#define GX_TEV_KCSEL_K0_B 0x18
#define GX_TEV_KCSEL_K0_A 0x1C
#define GX_TEV_KCSEL_K3_A 0x1F
#define GX_TEV_KASEL_1 0x00
#define GX_TEV_KASEL_1_8 0x07
#define GX_TEV_KASEL_K0_R 0x10
#define GX_TEV_KASEL_K0_G 0x14
#define GX_TEV_KASEL_K0_B 0x18
#define GX_TEV_KASEL_K0_A 0x1C
#define GX_TEV_KASEL_K3_A 0x1F
#define GX_MAX_TEVSWAP 4
#define GX_CH_RED 0
#define GX_CH_GREEN 1
#define GX_CH_BLUE 2
#define GX_CH_ALPHA 3

// GX_SetTevOp modes
#define GX_MODULATE 0
#define GX_REPLACE 3
#define GX_PASSCLR 4

// The rest, for the render targets (wii/gx_target_wii.c), with libogc's values
typedef float f32;
typedef f32 Mtx[3][4];
typedef f32 Mtx44[4][4];
typedef struct {
    void *image;
} GXTexObj;
#define GX_TF_RGB565 0x4
#define GX_TF_RGBA8 0x6
#define GX_TF_Z8 0x11
#define GX_TF_Z24X8 0x16
#define GX_CLAMP 0
#define GX_NEAR 0
#define GX_LINEAR 1
#define GX_TEXMAP1 1
#define GX_TEV_SWAP0 0
#define GX_TEVSTAGE1 1
#define GX_ZT_DISABLE 0
#define GX_ZT_REPLACE 2
#define GX_QUADS 0x80
#define GX_VTXFMT0 0
#define GX_VA_POS 9
#define GX_VA_TEX0 13
#define GX_POS_XYZ 1
#define GX_F32 4
#define GX_TEX_ST 1
#define GX_TG_MTX2x4 1
#define GX_TG_TEX0 4
#define GX_IDENTITY 60
#define GX_PNMTX0 0
#define GX_DIRECT 1
#define GX_ORTHOGRAPHIC 1
#define GX_BM_NONE 0
#define GX_BL_ZERO 0
#define GX_BL_ONE 1
#define GX_LO_CLEAR 0
#define GX_ALWAYS 7
#define GX_AOP_AND 0
#define GX_CULL_NONE 0

// What the stub logs: one line per call, "GX_Name arguments" (gx_stub_logf), in the order called.
// Memory a test names (gx_stub_name_memory) shows by name where a call takes it.
#define GX_STUB_LOG_LINES 512
#define GX_STUB_LOG_WIDTH 112
extern char g_gx_stub_log[GX_STUB_LOG_LINES][GX_STUB_LOG_WIDTH];
extern unsigned g_gx_stub_log_count;
void gx_stub_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void gx_stub_name_memory(const void *address, const char *name);
void gx_stub_clear_names(void);
const char *gx_stub_name(const void *address);

typedef struct {
    u8 texcoord;
    u32 texmap;
    u8 color;
    u8 ras_swap, tex_swap, kcsel, kasel;
    u8 cin[4], cop, cbias, cscale, cclamp, creg;
    u8 ain[4], aop, abias, ascale, aclamp, areg;
} GxStubStage;

typedef struct {
    GxStubStage stage[GX_MAX_TEVSTAGE];
    u8 num_stages, num_chans, num_texgens;
    u8 swap[4][4];
    GXColor kcolor[4];
    GXColor reg[4];
    unsigned calls;
} GxStub;

extern GxStub g_gx_stub;
void gx_stub_reset(void);

void GX_SetNumTevStages(u8 num);
void GX_SetNumChans(u8 num);
void GX_SetNumTexGens(u32 num);
void GX_SetTevOrder(u8 stage, u8 texcoord, u32 texmap, u8 color);
void GX_SetTevSwapMode(u8 stage, u8 ras_sel, u8 tex_sel);
void GX_SetTevSwapModeTable(u8 id, u8 r, u8 g, u8 b, u8 a);
void GX_SetTevKColor(u8 id, GXColor c);
void GX_SetTevColor(u8 id, GXColor c);
void GX_SetTevKColorSel(u8 stage, u8 sel);
void GX_SetTevKAlphaSel(u8 stage, u8 sel);
void GX_SetTevColorIn(u8 stage, u8 a, u8 b, u8 c, u8 d);
void GX_SetTevAlphaIn(u8 stage, u8 a, u8 b, u8 c, u8 d);
void GX_SetTevColorOp(u8 stage, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg);
void GX_SetTevAlphaOp(u8 stage, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg);
void GX_SetTevOp(u8 stage, u8 mode);

void GX_SetTexCopySrc(u16 left, u16 top, u16 w, u16 h);
void GX_SetTexCopyDst(u16 w, u16 h, u32 format, u8 mipmap);
void GX_CopyTex(void *dest, u8 clear);
void GX_PixModeSync(void);
void GX_SetCopyClear(GXColor color, u32 zvalue);
void GX_InvalidateTexAll(void);
void GX_SetScissor(u32 x, u32 y, u32 w, u32 h);
void GX_LoadProjectionMtx(Mtx44 m, u8 type);
void GX_SetViewport(f32 x, f32 y, f32 w, f32 h, f32 near, f32 far);
void GX_SetBlendMode(u8 type, u8 src, u8 dst, u8 op);
void GX_SetAlphaCompare(u8 comp0, u8 ref0, u8 aop, u8 comp1, u8 ref1);
void GX_SetCullMode(u8 mode);
void GX_SetNumIndStages(u8 num);
void GX_SetTevDirect(u8 stage);
void GX_SetZTexture(u8 op, u8 format, u32 bias);
void GX_SetZCompLoc(u8 before_tex);
void guMtxIdentity(Mtx m);
void GX_LoadPosMtxImm(Mtx m, u32 id);
void GX_SetCurrentMtx(u32 id);
void GX_ClearVtxDesc(void);
void GX_SetVtxDesc(u8 attr, u8 type);
void GX_SetVtxAttrFmt(u8 fmt, u32 attr, u32 comp_type, u32 comp_size, u32 frac);
void GX_SetTexCoordGen(u16 coord, u32 type, u32 src, u32 mtx);
void GX_Begin(u8 prim, u8 fmt, u16 count);
void GX_Position3f32(f32 x, f32 y, f32 z);
void GX_TexCoord2f32(f32 s, f32 t);
void GX_End(void);
void GX_InitTexObj(GXTexObj *obj, void *image, u16 w, u16 h, u8 format, u8 wrap_s, u8 wrap_t, u8 mipmap);
void GX_InitTexObjFilterMode(GXTexObj *obj, u8 min, u8 mag);
void GX_LoadTexObj(GXTexObj *obj, u8 map);
void GX_SetColorUpdate(u8 enable);
void GX_SetAlphaUpdate(u8 enable);
void GX_SetZMode(u8 enable, u8 func, u8 update);
