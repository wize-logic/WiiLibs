// SPDX-License-Identifier: MIT
// Applies a WlTevProgram (wl/platform/tev.h) to GX. Every enum of tev.h equals the libogc value,
// so a stage's fields go to the calls as they are. Only this directory may include <ogc/*.h>.
#include <ogc/gx.h>

#include "wl/platform/gx_shadow.h"
#include "wl/platform/tev.h"
#ifdef WL_GX_CHECK
#include "wl/platform/wii/gx_wii_internal.h"
#endif

_Static_assert(WL_TEV_CC_CPREV == GX_CC_CPREV && WL_TEV_CC_APREV == GX_CC_APREV && WL_TEV_CC_C0 == GX_CC_C0 &&
                   WL_TEV_CC_A0 == GX_CC_A0 && WL_TEV_CC_C1 == GX_CC_C1 && WL_TEV_CC_A1 == GX_CC_A1 &&
                   WL_TEV_CC_C2 == GX_CC_C2 && WL_TEV_CC_A2 == GX_CC_A2 && WL_TEV_CC_TEXC == GX_CC_TEXC &&
                   WL_TEV_CC_TEXA == GX_CC_TEXA && WL_TEV_CC_RASC == GX_CC_RASC && WL_TEV_CC_RASA == GX_CC_RASA &&
                   WL_TEV_CC_ONE == GX_CC_ONE && WL_TEV_CC_HALF == GX_CC_HALF && WL_TEV_CC_KONST == GX_CC_KONST &&
                   WL_TEV_CC_ZERO == GX_CC_ZERO,
               "WL_TEV_CC_* are GX_CC_*");
_Static_assert(WL_TEV_CA_APREV == GX_CA_APREV && WL_TEV_CA_A0 == GX_CA_A0 && WL_TEV_CA_A1 == GX_CA_A1 &&
                   WL_TEV_CA_A2 == GX_CA_A2 && WL_TEV_CA_TEXA == GX_CA_TEXA && WL_TEV_CA_RASA == GX_CA_RASA &&
                   WL_TEV_CA_KONST == GX_CA_KONST && WL_TEV_CA_ZERO == GX_CA_ZERO,
               "WL_TEV_CA_* are GX_CA_*");
_Static_assert(WL_TEV_ADD == GX_TEV_ADD && WL_TEV_SUB == GX_TEV_SUB && WL_TEV_COMP_R8_GT == GX_TEV_COMP_R8_GT &&
                   WL_TEV_COMP_R8_EQ == GX_TEV_COMP_R8_EQ && WL_TEV_COMP_GR16_GT == GX_TEV_COMP_GR16_GT &&
                   WL_TEV_COMP_GR16_EQ == GX_TEV_COMP_GR16_EQ && WL_TEV_COMP_BGR24_GT == GX_TEV_COMP_BGR24_GT &&
                   WL_TEV_COMP_BGR24_EQ == GX_TEV_COMP_BGR24_EQ && WL_TEV_COMP_RGB8_GT == GX_TEV_COMP_RGB8_GT &&
                   WL_TEV_COMP_RGB8_EQ == GX_TEV_COMP_RGB8_EQ && WL_TEV_COMP_A8_GT == GX_TEV_COMP_A8_GT &&
                   WL_TEV_COMP_A8_EQ == GX_TEV_COMP_A8_EQ,
               "WL_TEV ops are GX_TEV_*");
_Static_assert(WL_TEV_BIAS_ZERO == GX_TB_ZERO && WL_TEV_BIAS_ADDHALF == GX_TB_ADDHALF &&
                   WL_TEV_BIAS_SUBHALF == GX_TB_SUBHALF && WL_TEV_BIAS_COUNT == GX_MAX_TEVBIAS,
               "biases are GX_TB_*");
_Static_assert(WL_TEV_SCALE_1 == GX_CS_SCALE_1 && WL_TEV_SCALE_2 == GX_CS_SCALE_2 && WL_TEV_SCALE_4 == GX_CS_SCALE_4 &&
                   WL_TEV_DIVIDE_2 == GX_CS_DIVIDE_2 && WL_TEV_SCALE_COUNT == GX_MAX_TEVSCALE,
               "scales are GX_CS_*");
_Static_assert(WL_TEV_PREV == GX_TEVPREV && WL_TEV_REG0 == GX_TEVREG0 && WL_TEV_REG1 == GX_TEVREG1 &&
                   WL_TEV_REG2 == GX_TEVREG2 && WL_TEV_REG_COUNT == GX_MAX_TEVREG,
               "registers are GX_TEVREG*");
_Static_assert(WL_TEV_CH_COLOR0A0 == GX_COLOR0A0 && WL_TEV_CH_COLOR1A1 == GX_COLOR1A1 && WL_TEV_CH_ZERO == GX_COLORZERO &&
                   WL_TEV_NONE == GX_COLORNULL && WL_TEV_NONE == GX_TEXCOORDNULL && WL_TEV_NONE == GX_TEXMAP_NULL,
               "channels and nulls are GX's");
_Static_assert(WL_TEV_SWAP_R == GX_CH_RED && WL_TEV_SWAP_G == GX_CH_GREEN && WL_TEV_SWAP_B == GX_CH_BLUE &&
                   WL_TEV_SWAP_A == GX_CH_ALPHA && WL_TEV_SWAP_TABLES == GX_MAX_TEVSWAP,
               "swap channels are GX_CH_*");
_Static_assert(WL_TEV_KSEL_1 == GX_TEV_KCSEL_1 && WL_TEV_KSEL_1_8 == GX_TEV_KCSEL_1_8 && WL_TEV_KSEL_1 == GX_TEV_KASEL_1 &&
                   WL_TEV_KSEL_1_8 == GX_TEV_KASEL_1_8 && WL_TEV_KSEL_1_2 == GX_TEV_KCSEL_1_2 &&
                   0x0C == GX_TEV_KCSEL_K0 && 0x10 == GX_TEV_KCSEL_K0_R && 0x14 == GX_TEV_KCSEL_K0_G &&
                   0x18 == GX_TEV_KCSEL_K0_B && 0x1C == GX_TEV_KCSEL_K0_A && 0x1F == GX_TEV_KCSEL_K3_A &&
                   0x10 == GX_TEV_KASEL_K0_R && 0x14 == GX_TEV_KASEL_K0_G && 0x18 == GX_TEV_KASEL_K0_B &&
                   0x1C == GX_TEV_KASEL_K0_A && 0x1F == GX_TEV_KASEL_K3_A,
               "konst selections are GX_TEV_KCSEL_* / GX_TEV_KASEL_*");
_Static_assert(WL_TEV_MAX_STAGES == GX_MAX_TEVSTAGE && WL_TEV_KONST_SLOTS == GX_KCOLOR_MAX &&
                   WL_TEV_TEXMAPS == GX_MAX_TEXMAP,
               "limits are GX's");

static GXColor unpack(uint32_t rgba) {
    GXColor c = {(u8)(rgba >> 24), (u8)(rgba >> 16), (u8)(rgba >> 8), (u8)rgba};
    return c;
}

// the last program's orders as applied, their maps the logical ones (gx_shadow.h: texture maps as a cache)
static struct {
    u8 texcoord, texmap, channel;
} g_order[WL_TEV_MAX_STAGES];
static u8 g_order_count;

#ifdef WL_GX_CHECK
// the program GX holds, for the draws' check (gx_direct_wii.c; its stages' maps those they read)
static WlTevProgram g_last;
static bool g_last_set;

const WlTevProgram *wl_gx_wii_tev_last(void) {
    if (g_last_set) {
        for (u8 i = 0; i < g_last.stage_count; i++) {
            g_last.stages[i].texmap = g_order[i].texmap < WL_TEV_TEXMAPS ? (u8)wl_gx_shadow_map_of(g_order[i].texmap) : g_order[i].texmap;
        }
    }
    return g_last_set ? &g_last : NULL;
}
#endif

static inline u8 map_read(u8 texmap) {
    return texmap < WL_TEV_TEXMAPS ? (u8)wl_gx_shadow_map_of(texmap) : texmap;
}

void wl_tev_maps_follow(void) {
    if (!wl_gx_shadow_maps_moved()) {
        return;
    }
    wl_gx_shadow_maps_followed();
    for (u8 i = 0; i < g_order_count; i++) {
        u8 m = map_read(g_order[i].texmap);

        if (g_order[i].texmap < WL_TEV_TEXMAPS &&
            wl_tev_shadow_order_moved(i, g_order[i].texcoord, m, g_order[i].channel)) {
            GX_SetTevOrder((u8)(GX_TEVSTAGE0 + i), g_order[i].texcoord, m, g_order[i].channel);
        }
    }
}

void wl_tev_apply_unchecked_ex(const WlTevProgram *p, uint32_t flags) {
    WlTevSend send;

#ifdef WL_GX_CHECK
    g_last = *p;
    g_last_set = true;
#endif
    g_order_count = p->stage_count;
    for (u8 i = 0; i < p->stage_count; i++) {
        g_order[i].texcoord = p->stages[i].texcoord;
        g_order[i].texmap = p->stages[i].texmap;
        g_order[i].channel = p->stages[i].channel;
    }

    // (what to send: everything, or with WL_TEV_APPLY_CHANGES what GX does not hold already)
    wl_tev_shadow_plan(p, flags, &send);
    for (u8 t = 0; t < WL_TEV_SWAP_TABLES; t++) {
        if ((send.swaps & (1u << t)) != 0) {
            GX_SetTevSwapModeTable(t, p->swap[t][0], p->swap[t][1], p->swap[t][2], p->swap[t][3]);
        }
    }
    for (u8 k = 0; k < WL_TEV_KONST_SLOTS; k++) {
        if ((send.konst & (1u << k)) != 0) {
            GX_SetTevKColor((u8)(GX_KCOLOR0 + k), unpack(p->konst[k]));
        }
    }
    for (u8 r = 0; r < WL_TEV_REG_COUNT; r++) {
        if ((send.preload & (1u << r)) != 0) {
            GX_SetTevColor(r, unpack(p->preload[r]));
        }
    }
    if (send.stage_count) {
        GX_SetNumTevStages(p->stage_count);
    }
    for (u8 i = 0; i < p->stage_count; i++) {
        const WlTevStage *s = &p->stages[i];
        u8 st = (u8)(GX_TEVSTAGE0 + i), calls = send.stages[i];
        if (calls == 0) {
            continue;
        }
        if (calls & WL_TEV_SEND_ORDER) {
            GX_SetTevOrder(st, s->texcoord, map_read(s->texmap), s->channel);
        }
        if (calls & WL_TEV_SEND_SWAP) {
            GX_SetTevSwapMode(st, s->ras_swap, s->tex_swap);
        }
        if (calls & WL_TEV_SEND_KCSEL) {
            GX_SetTevKColorSel(st, s->kcsel);
        }
        if (calls & WL_TEV_SEND_KASEL) {
            GX_SetTevKAlphaSel(st, s->kasel);
        }
        if (calls & WL_TEV_SEND_COLOR_IN) {
            GX_SetTevColorIn(st, s->color.in[0], s->color.in[1], s->color.in[2], s->color.in[3]);
        }
        if (calls & WL_TEV_SEND_COLOR_OP) {
            GX_SetTevColorOp(st, s->color.op, s->color.bias, s->color.scale, s->color.clamp, s->color.dest);
        }
        if (calls & WL_TEV_SEND_ALPHA_IN) {
            GX_SetTevAlphaIn(st, s->alpha.in[0], s->alpha.in[1], s->alpha.in[2], s->alpha.in[3]);
        }
        if (calls & WL_TEV_SEND_ALPHA_OP) {
            GX_SetTevAlphaOp(st, s->alpha.op, s->alpha.bias, s->alpha.scale, s->alpha.clamp, s->alpha.dest);
        }
    }
}
