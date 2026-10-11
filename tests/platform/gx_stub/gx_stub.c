// SPDX-License-Identifier: MIT
// The recording calls of gx_stub/ogc/gx.h. GX_SetTevOp follows libogc's: the first stage's default
// input is the rasterised colour, later stages' the previous stage.
#include <ogc/gx.h>
#include <string.h>

GxStub g_gx_stub;

void gx_stub_reset(void) {
    memset(&g_gx_stub, 0, sizeof g_gx_stub);
    // GX_Init's swap tables: RGBA, RRRA, GGGA, BBBA
    for (u8 t = 0; t < 4; t++) {
        for (u8 i = 0; i < 4; i++) {
            g_gx_stub.swap[t][i] = i == 3 ? GX_CH_ALPHA : (t == 0 ? i : (u8)(t - 1));
        }
    }
}

void GX_SetNumTevStages(u8 num) { g_gx_stub.num_stages = num; g_gx_stub.calls++; }
void GX_SetNumChans(u8 num) { g_gx_stub.num_chans = num; g_gx_stub.calls++; }
void GX_SetNumTexGens(u32 num) { g_gx_stub.num_texgens = (u8)num; g_gx_stub.calls++; }
void GX_SetTevOrder(u8 stage, u8 texcoord, u32 texmap, u8 color) {
    GxStubStage *s = &g_gx_stub.stage[stage];
    s->texcoord = texcoord;
    s->texmap = texmap;
    s->color = color;
    g_gx_stub.calls++;
}
void GX_SetTevSwapMode(u8 stage, u8 ras_sel, u8 tex_sel) {
    g_gx_stub.stage[stage].ras_swap = ras_sel;
    g_gx_stub.stage[stage].tex_swap = tex_sel;
    g_gx_stub.calls++;
}
void GX_SetTevSwapModeTable(u8 id, u8 r, u8 g, u8 b, u8 a) {
    g_gx_stub.swap[id][0] = r;
    g_gx_stub.swap[id][1] = g;
    g_gx_stub.swap[id][2] = b;
    g_gx_stub.swap[id][3] = a;
    g_gx_stub.calls++;
}
void GX_SetTevKColor(u8 id, GXColor c) { g_gx_stub.kcolor[id] = c; g_gx_stub.calls++; }
void GX_SetTevColor(u8 id, GXColor c) { g_gx_stub.reg[id] = c; g_gx_stub.calls++; }
void GX_SetTevKColorSel(u8 stage, u8 sel) { g_gx_stub.stage[stage].kcsel = sel; g_gx_stub.calls++; }
void GX_SetTevKAlphaSel(u8 stage, u8 sel) { g_gx_stub.stage[stage].kasel = sel; g_gx_stub.calls++; }
void GX_SetTevColorIn(u8 stage, u8 a, u8 b, u8 c, u8 d) {
    u8 *in = g_gx_stub.stage[stage].cin;
    in[0] = a; in[1] = b; in[2] = c; in[3] = d;
    g_gx_stub.calls++;
}
void GX_SetTevAlphaIn(u8 stage, u8 a, u8 b, u8 c, u8 d) {
    u8 *in = g_gx_stub.stage[stage].ain;
    in[0] = a; in[1] = b; in[2] = c; in[3] = d;
    g_gx_stub.calls++;
}
void GX_SetTevColorOp(u8 stage, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg) {
    GxStubStage *s = &g_gx_stub.stage[stage];
    s->cop = op; s->cbias = bias; s->cscale = scale; s->cclamp = clamp; s->creg = reg;
    g_gx_stub.calls++;
}
void GX_SetTevAlphaOp(u8 stage, u8 op, u8 bias, u8 scale, u8 clamp, u8 reg) {
    GxStubStage *s = &g_gx_stub.stage[stage];
    s->aop = op; s->abias = bias; s->ascale = scale; s->aclamp = clamp; s->areg = reg;
    g_gx_stub.calls++;
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
