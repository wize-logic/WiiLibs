// SPDX-License-Identifier: MIT
// The recording calls of gx_stub/ogc/gx.h. GX_SetTevOp follows libogc's: the first stage's default
// input is the rasterised colour, later stages' the previous stage. Every call is also written to a
// log of text lines (gx_stub_logf), for tests of the order of GX calls.
#include <ogc/gx.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

GxStub g_gx_stub;
char g_gx_stub_log[GX_STUB_LOG_LINES][GX_STUB_LOG_WIDTH];
unsigned g_gx_stub_log_count;

void gx_stub_logf(const char *fmt, ...) {
    if (g_gx_stub_log_count >= GX_STUB_LOG_LINES) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(g_gx_stub_log[g_gx_stub_log_count++], GX_STUB_LOG_WIDTH, fmt, ap);
    va_end(ap);
}

void gx_stub_reset(void) {
    memset(&g_gx_stub, 0, sizeof g_gx_stub);
    g_gx_stub_log_count = 0;
    // GX_Init's swap tables: RGBA, RRRA, GGGA, BBBA
    for (u8 t = 0; t < 4; t++) {
        for (u8 i = 0; i < 4; i++) {
            g_gx_stub.swap[t][i] = i == 3 ? GX_CH_ALPHA : (t == 0 ? i : (u8)(t - 1));
        }
    }
}

// Memory the test names (gx_stub_name_memory) shows by its name in the log.
static struct {
    const void *address;
    const char *name;
} g_names[8];
static unsigned g_name_count;

void gx_stub_name_memory(const void *address, const char *name) {
    if (g_name_count < sizeof g_names / sizeof g_names[0]) {
        g_names[g_name_count].address = address;
        g_names[g_name_count].name = name;
        g_name_count++;
    }
}

void gx_stub_clear_names(void) {
    g_name_count = 0;
}

const char *gx_stub_name(const void *address) {
    for (unsigned i = 0; i < g_name_count; i++) {
        if (g_names[i].address == address) {
            return g_names[i].name;
        }
    }
    return "?";
}

void GX_SetNumTevStages(u8 num) { g_gx_stub.num_stages = num; g_gx_stub.calls++; gx_stub_logf("GX_SetNumTevStages %u", num); }
void GX_SetNumChans(u8 num) { g_gx_stub.num_chans = num; g_gx_stub.calls++; gx_stub_logf("GX_SetNumChans %u", num); }
void GX_SetNumTexGens(u32 num) { g_gx_stub.num_texgens = (u8)num; g_gx_stub.calls++; gx_stub_logf("GX_SetNumTexGens %u", (unsigned)num); }
void GX_SetTevOrder(u8 stage, u8 texcoord, u32 texmap, u8 color) {
    GxStubStage *s = &g_gx_stub.stage[stage];
    s->texcoord = texcoord;
    s->texmap = texmap;
    s->color = color;
    g_gx_stub.calls++;
    gx_stub_logf("GX_SetTevOrder %u %u %u %u", stage, texcoord, (unsigned)texmap, color);
}
void GX_SetTevSwapMode(u8 stage, u8 ras_sel, u8 tex_sel) {
    g_gx_stub.stage[stage].ras_swap = ras_sel;
    g_gx_stub.stage[stage].tex_swap = tex_sel;
    g_gx_stub.calls++;
    gx_stub_logf("GX_SetTevSwapMode %u %u %u", stage, ras_sel, tex_sel);
}
void GX_SetTevSwapModeTable(u8 id, u8 r, u8 g, u8 b, u8 a) {
    g_gx_stub.swap[id][0] = r;
    g_gx_stub.swap[id][1] = g;
    g_gx_stub.swap[id][2] = b;
    g_gx_stub.swap[id][3] = a;
    g_gx_stub.calls++;
    gx_stub_logf("GX_SetTevSwapModeTable %u %u %u %u %u", id, r, g, b, a);
}
void GX_SetTevKColor(u8 id, GXColor c) { g_gx_stub.kcolor[id] = c; g_gx_stub.calls++; gx_stub_logf("GX_SetTevKColor %u %u %u %u %u", id, c.r, c.g, c.b, c.a); }
void GX_SetTevColor(u8 id, GXColor c) { g_gx_stub.reg[id] = c; g_gx_stub.calls++; gx_stub_logf("GX_SetTevColor %u %u %u %u %u", id, c.r, c.g, c.b, c.a); }
void GX_SetTevKColorSel(u8 stage, u8 sel) { g_gx_stub.stage[stage].kcsel = sel; g_gx_stub.calls++; gx_stub_logf("GX_SetTevKColorSel %u %u", stage, sel); }
void GX_SetTevKAlphaSel(u8 stage, u8 sel) { g_gx_stub.stage[stage].kasel = sel; g_gx_stub.calls++; gx_stub_logf("GX_SetTevKAlphaSel %u %u", stage, sel); }
void GX_SetTevColorIn(u8 stage, u8 a, u8 b, u8 c, u8 d) {
    u8 *in = g_gx_stub.stage[stage].cin;
    in[0] = a; in[1] = b; in[2] = c; in[3] = d;
    g_gx_stub.calls++;
    gx_stub_logf("GX_SetTevColorIn %u %u %u %u %u", stage, a, b, c, d);
}
void GX_SetTevAlphaIn(u8 stage, u8 a, u8 b, u8 c, u8 d) {
    u8 *in = g_gx_stub.stage[stage].ain;
    in[0] = a; in[1] = b; in[2] = c; in[3] = d;
    g_gx_stub.calls++;
    gx_stub_logf("GX_SetTevAlphaIn %u %u %u %u %u", stage, a, b, c, d);
}
void GX_SetTevColorOp(u8 stage, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg) {
    GxStubStage *s = &g_gx_stub.stage[stage];
    s->cop = op; s->cbias = bias; s->cscale = scale; s->cclamp = clamp; s->creg = reg;
    g_gx_stub.calls++;
    gx_stub_logf("GX_SetTevColorOp %u %u %u %u %u %u", stage, op, bias, scale, clamp, reg);
}
void GX_SetTevAlphaOp(u8 stage, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg) {
    GxStubStage *s = &g_gx_stub.stage[stage];
    s->aop = op; s->abias = bias; s->ascale = scale; s->aclamp = clamp; s->areg = reg;
    g_gx_stub.calls++;
    gx_stub_logf("GX_SetTevAlphaOp %u %u %u %u %u %u", stage, op, bias, scale, clamp, reg);
}
void GX_SetTevOp(u8 stage, u8 mode) {
    u8 dc = stage == GX_TEVSTAGE0 ? GX_CC_RASC : GX_CC_CPREV;
    u8 da = stage == GX_TEVSTAGE0 ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
    case GX_MODULATE:
        GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, dc, GX_CC_ZERO);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, da, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    default: // GX_PASSCLR
        GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, dc);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, da);
        break;
    }
    GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

// ---- the rest of what the render targets call (log only) ---------------------------------------------

void GX_SetTexCopySrc(u16 left, u16 top, u16 w, u16 h) { gx_stub_logf("GX_SetTexCopySrc %u %u %u %u", left, top, w, h); }
void GX_SetTexCopyDst(u16 w, u16 h, u32 format, u8 mipmap) { gx_stub_logf("GX_SetTexCopyDst %u %u %#x %u", w, h, (unsigned)format, mipmap); }
void GX_CopyTex(void *dest, u8 clear) { gx_stub_logf("GX_CopyTex %s %u", gx_stub_name(dest), clear); }
void GX_PixModeSync(void) { gx_stub_logf("GX_PixModeSync"); }
void GX_SetCopyClear(GXColor color, u32 zvalue) {
    gx_stub_logf("GX_SetCopyClear %u %u %u %u 0x%X", color.r, color.g, color.b, color.a, (unsigned)zvalue);
}
void GX_InvalidateTexAll(void) { gx_stub_logf("GX_InvalidateTexAll"); }
void GX_SetScissor(u32 x, u32 y, u32 w, u32 h) { gx_stub_logf("GX_SetScissor %u %u %u %u", (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h); }
void GX_LoadProjectionMtx(Mtx44 m, u8 type) {
    gx_stub_logf("GX_LoadProjectionMtx %s %g %g %g %g %g %g %g", type == GX_ORTHOGRAPHIC ? "ortho" : "persp",
                 (double)m[0][0], (double)m[0][3], (double)m[1][1], (double)m[1][3], (double)m[2][2], (double)m[2][3],
                 (double)m[3][3]);
}
void GX_SetViewport(f32 x, f32 y, f32 w, f32 h, f32 near, f32 far) {
    gx_stub_logf("GX_SetViewport %g %g %g %g %g %g", (double)x, (double)y, (double)w, (double)h, (double)near, (double)far);
}
void GX_SetBlendMode(u8 type, u8 src, u8 dst, u8 op) { gx_stub_logf("GX_SetBlendMode %u %u %u %u", type, src, dst, op); }
void GX_SetAlphaCompare(u8 comp0, u8 ref0, u8 aop, u8 comp1, u8 ref1) {
    gx_stub_logf("GX_SetAlphaCompare %u %u %u %u %u", comp0, ref0, aop, comp1, ref1);
}
void GX_SetCullMode(u8 mode) { gx_stub_logf("GX_SetCullMode %u", mode); }
void GX_SetNumIndStages(u8 num) { gx_stub_logf("GX_SetNumIndStages %u", num); }
void GX_SetTevDirect(u8 stage) { gx_stub_logf("GX_SetTevDirect %u", stage); }
void GX_SetZTexture(u8 op, u8 format, u32 bias) { gx_stub_logf("GX_SetZTexture %u %#x %u", op, format, (unsigned)bias); }
void GX_SetZCompLoc(u8 before_tex) { gx_stub_logf("GX_SetZCompLoc %u", before_tex); }
void guMtxIdentity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}
void GX_LoadPosMtxImm(Mtx m, u32 id) {
    bool identity = m[0][0] == 1.0f && m[1][1] == 1.0f && m[2][2] == 1.0f && m[0][1] == 0.0f && m[0][3] == 0.0f;
    gx_stub_logf("GX_LoadPosMtxImm %s %u", identity ? "identity" : "other", (unsigned)id);
}
void GX_SetCurrentMtx(u32 id) { gx_stub_logf("GX_SetCurrentMtx %u", (unsigned)id); }
void GX_ClearVtxDesc(void) { gx_stub_logf("GX_ClearVtxDesc"); }
void GX_SetVtxDesc(u8 attr, u8 type) { gx_stub_logf("GX_SetVtxDesc %u %u", attr, type); }
void GX_SetVtxAttrFmt(u8 fmt, u32 attr, u32 comp_type, u32 comp_size, u32 frac) {
    gx_stub_logf("GX_SetVtxAttrFmt %u %u %u %u %u", fmt, (unsigned)attr, (unsigned)comp_type, (unsigned)comp_size,
                 (unsigned)frac);
}
void GX_SetTexCoordGen(u16 coord, u32 type, u32 src, u32 mtx) {
    gx_stub_logf("GX_SetTexCoordGen %u %u %u %u", coord, (unsigned)type, (unsigned)src, (unsigned)mtx);
}
void GX_Begin(u8 prim, u8 fmt, u16 count) { gx_stub_logf("GX_Begin %#x %u %u", prim, fmt, count); }
void GX_Position3f32(f32 x, f32 y, f32 z) { gx_stub_logf("GX_Position3f32 %g %g %g", (double)x, (double)y, (double)z); }
void GX_TexCoord2f32(f32 s, f32 t) { gx_stub_logf("GX_TexCoord2f32 %g %g", (double)s, (double)t); }
void GX_End(void) { gx_stub_logf("GX_End"); }
void GX_InitTexObj(GXTexObj *obj, void *image, u16 w, u16 h, u8 format, u8 wrap_s, u8 wrap_t, u8 mipmap) {
    obj->image = image;
    gx_stub_logf("GX_InitTexObj %s %u %u %#x %u %u %u", gx_stub_name(image), w, h, format, wrap_s, wrap_t, mipmap);
}
void GX_InitTexObjFilterMode(GXTexObj *obj, u8 min, u8 mag) {
    (void)obj;
    gx_stub_logf("GX_InitTexObjFilterMode %u %u", min, mag);
}
void GX_LoadTexObj(GXTexObj *obj, u8 map) { gx_stub_logf("GX_LoadTexObj %s %u", gx_stub_name(obj->image), map); }
void GX_SetColorUpdate(u8 enable) { gx_stub_logf("GX_SetColorUpdate %u", enable); }
void GX_SetAlphaUpdate(u8 enable) { gx_stub_logf("GX_SetAlphaUpdate %u", enable); }
void GX_SetZMode(u8 enable, u8 func, u8 update) { gx_stub_logf("GX_SetZMode %u %u %u", enable, func, update); }
