// SPDX-License-Identifier: MIT
// wl/platform/wii/tev_wii.c compiled against a recording stand-in for libogc's GX
// (tests/platform/gx_stub): what wl_tev_apply writes, and that the fixed recipes, now programs,
// leave GX in the state the hand-written recipe code (kept below as the reference) left it in.
#include <ogc/gx.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/gx_shadow.h"
#include "wl/platform/tev.h"

// ---- the reference: wl_gx_set_recipe as it was before the recipes became programs ----------------

static void ref_tev_stage(u8 st, u8 ca, u8 cb, u8 cc, u8 cd, u8 aa, u8 ab, u8 ac, u8 ad) {
    GX_SetTevColorIn(st, ca, cb, cc, cd);
    GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(st, aa, ab, ac, ad);
    GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void ref_set_recipe(WlGxRecipe recipe) {
    GX_SetNumTevStages(1);
    switch (recipe) {
    case WL_GX_RECIPE_TEX_REPLACE:
        GX_SetNumChans(0);
        GX_SetNumTexGens(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
        break;
    case WL_GX_RECIPE_TEX_MODULATE:
    case WL_GX_RECIPE_TEX_LIT:
        GX_SetNumChans(1);
        GX_SetNumTexGens(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        break;
    case WL_GX_RECIPE_TEX_TINT:
        GX_SetNumChans(1);
        GX_SetNumTexGens(1);
        GX_SetNumTevStages(2);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        GX_SetTevOrder(1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        GX_SetTevKColorSel(1, GX_TEV_KCSEL_K0);
        GX_SetTevKAlphaSel(1, GX_TEV_KASEL_K0_A);
        ref_tev_stage(1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO, GX_CA_ZERO, GX_CA_APREV, GX_CA_KONST,
                      GX_CA_ZERO);
        break;
    case WL_GX_RECIPE_ADDITIVE:
        GX_SetNumChans(1);
        GX_SetNumTexGens(1);
        GX_SetNumTevStages(2);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
        ref_tev_stage(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO, GX_CA_ZERO, GX_CA_ZERO,
                      GX_CA_ZERO, GX_CA_KONST);
        GX_SetTevOrder(1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        ref_tev_stage(1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASA, GX_CC_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
                      GX_CA_APREV);
        break;
    case WL_GX_RECIPE_UI:
        GX_SetNumChans(1);
        GX_SetNumTexGens(1);
        GX_SetNumTevStages(2);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        GX_SetTevOrder(1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        GX_SetTevKAlphaSel(1, GX_TEV_KASEL_K0_A);
        ref_tev_stage(1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV, GX_CA_ZERO, GX_CA_APREV, GX_CA_KONST,
                      GX_CA_ZERO);
        break;
    case WL_GX_RECIPE_TEXT:
        GX_SetNumChans(1);
        GX_SetNumTexGens(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        ref_tev_stage(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC, GX_CA_ZERO, GX_CA_TEXA,
                      GX_CA_RASA, GX_CA_ZERO);
        break;
    case WL_GX_RECIPE_KONST:
        GX_SetNumChans(0);
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
        GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
        ref_tev_stage(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST, GX_CA_ZERO, GX_CA_ZERO,
                      GX_CA_ZERO, GX_CA_KONST);
        break;
    default:
        GX_SetNumChans(1);
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        break;
    }
}

// What wl_gx_set_recipe does now.
static void new_set_recipe(WlGxRecipe recipe) {
    WlTevProgram prog;
    wl_tev_recipe(&prog, recipe);
    wl_tev_apply_unchecked(&prog);
    GX_SetNumChans((u8)wl_tev_channels_used(&prog));
    GX_SetNumTexGens(wl_tev_texcoords_used(&prog));
}

TEST recipes_leave_gx_as_the_hand_written_code_did(void) {
    for (int r = 0; r < WL_GX_RECIPE_COUNT; r++) {
        gx_stub_reset();
        ref_set_recipe((WlGxRecipe)r);
        GxStub want = g_gx_stub;
        gx_stub_reset();
        new_set_recipe((WlGxRecipe)r);
        const GxStub *got = &g_gx_stub;
        ASSERT_EQ_FMT((int)want.num_stages, (int)got->num_stages, "%d");
        ASSERT_EQ_FMT((int)want.num_chans, (int)got->num_chans, "%d");
        ASSERT_EQ_FMT((int)want.num_texgens, (int)got->num_texgens, "%d");
        for (int s = 0; s < want.num_stages; s++) {
            ASSERT_EQm("stage state", 0, memcmp(&want.stage[s], &got->stage[s], sizeof want.stage[s]));
        }
        ASSERT_EQm("swap tables", 0, memcmp(want.swap, got->swap, sizeof want.swap));
        // (the recipes leave the konst colour to wl_gx_set_konst)
        ASSERT_EQm("konst colours", 0, memcmp(want.kcolor, got->kcolor, sizeof want.kcolor));
    }
    PASS();
}

TEST apply_writes_every_field(void) {
    WlTevProgram p;
    wl_tev_program_init(&p);
    wl_tev_set_swap(&p, 2, WL_TEV_SWAP_A, WL_TEV_SWAP_B, WL_TEV_SWAP_G, WL_TEV_SWAP_R);
    ASSERT_EQ(0, wl_tev_konst_slot(&p, 0)); // slot 0 holds colour 0
    ASSERT_EQ(1, wl_tev_konst_slot(&p, 0x11223344u));
    wl_tev_konst_external(&p, 3, 0);
    wl_tev_preload(&p, WL_TEV_REG2, 0xAABBCCDDu);
    WlTevStage *s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, 5, 6, WL_TEV_CH_COLOR1A1);
    s->ras_swap = 2;
    s->tex_swap = 1;
    s->kcsel = wl_tev_kcsel(1, WL_TEV_K_G);
    s->kasel = wl_tev_kasel(1, WL_TEV_K_B);
    wl_tev_stage_color(s, WL_TEV_CC_C2, WL_TEV_CC_TEXC, WL_TEV_CC_RASA, WL_TEV_CC_KONST, WL_TEV_SUB,
                       WL_TEV_BIAS_ADDHALF, WL_TEV_SCALE_2, false, WL_TEV_REG1);
    wl_tev_stage_alpha(s, WL_TEV_CA_A2, WL_TEV_CA_TEXA, WL_TEV_CA_RASA, WL_TEV_CA_KONST, WL_TEV_COMP_A8_EQ, 0, 0, true,
                       WL_TEV_REG2);
    s = wl_tev_add_stage(&p);
    wl_tev_stage_order(s, WL_TEV_NONE, WL_TEV_NONE, WL_TEV_NONE);
    wl_tev_stage_color(s, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_ZERO, WL_TEV_CC_C1, WL_TEV_ADD, 0, 0, true,
                       WL_TEV_PREV);

    gx_stub_reset();
    g_gx_stub.kcolor[3].r = 77; // not the program's: external
    ASSERT_EQ(WL_OK, wl_tev_apply(&p));
    const GxStub *g = &g_gx_stub;
    ASSERT_EQ(2, g->num_stages);
    const GxStubStage *a = &g->stage[0];
    ASSERT_EQ(5, a->texcoord);
    ASSERT_EQ(6u, a->texmap);
    ASSERT_EQ(GX_COLOR1A1, a->color);
    ASSERT_EQ(2, a->ras_swap);
    ASSERT_EQ(1, a->tex_swap);
    ASSERT_EQ(0x15, a->kcsel);
    ASSERT_EQ(0x19, a->kasel);
    ASSERT_EQ(GX_CC_C2, a->cin[0]);
    ASSERT_EQ(GX_CC_TEXC, a->cin[1]);
    ASSERT_EQ(GX_CC_RASA, a->cin[2]);
    ASSERT_EQ(GX_CC_KONST, a->cin[3]);
    ASSERT_EQ(GX_TEV_SUB, a->cop);
    ASSERT_EQ(GX_TB_ADDHALF, a->cbias);
    ASSERT_EQ(GX_CS_SCALE_2, a->cscale);
    ASSERT_EQ(0, a->cclamp);
    ASSERT_EQ(GX_TEVREG1, a->creg);
    ASSERT_EQ(GX_CA_A2, a->ain[0]);
    ASSERT_EQ(GX_CA_KONST, a->ain[3]);
    ASSERT_EQ(GX_TEV_COMP_A8_EQ, a->aop);
    ASSERT_EQ(1, a->aclamp);
    ASSERT_EQ(GX_TEVREG2, a->areg);
    const GxStubStage *b = &g->stage[1];
    ASSERT_EQ(GX_TEXCOORDNULL, b->texcoord);
    ASSERT_EQ(GX_TEXMAP_NULL, b->texmap);
    ASSERT_EQ(GX_COLORNULL, b->color);
    ASSERT_EQ(GX_CC_C1, b->cin[3]);
    ASSERT_EQ(GX_TEVPREV, b->creg);
    // swap table 2 (used by stage 0) is written, and 0, 1 (stage 1 and 0's tex), but not 3
    ASSERT_EQ(WL_TEV_SWAP_A, g->swap[2][0]);
    ASSERT_EQ(WL_TEV_SWAP_R, g->swap[2][3]);
    // konsts 0 and 1 are the program's, 3 is external and not written
    ASSERT_EQ(0x11, g->kcolor[1].r);
    ASSERT_EQ(0x44, g->kcolor[1].a);
    ASSERT_EQ(0, g->kcolor[0].r + g->kcolor[0].a); // (slot 0 holds the colour 0)
    ASSERT_EQ(77, g->kcolor[3].r);
    // the preload of REG2
    ASSERT_EQ(0xAA, g->reg[WL_TEV_REG2].r);
    ASSERT_EQ(0xDD, g->reg[WL_TEV_REG2].a);
    ASSERT_EQ(0, g->reg[WL_TEV_REG1].r);
    PASS();
}

TEST apply_refuses_an_invalid_program(void) {
    WlTevProgram p;
    wl_tev_recipe(&p, WL_GX_RECIPE_TEXT);
    p.stages[0].color.in[1] = 77;
    gx_stub_reset();
    ASSERT_EQ(WL_ERR_ARG, wl_tev_apply(&p));
    ASSERT_EQ(0u, g_gx_stub.calls); // nothing was written
    PASS();
}

TEST unused_swap_tables_are_left_alone(void) {
    WlTevProgram p;
    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_MODULATE);
    gx_stub_reset();
    g_gx_stub.swap[3][0] = 99; // a table of the application's, no stage reads through it
    wl_tev_apply_unchecked(&p);
    ASSERT_EQ(99, g_gx_stub.swap[3][0]);
    PASS();
}

TEST skip_swaps_leaves_the_tables_to_the_caller(void) {
    WlTevProgram p;
    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_MODULATE);
    wl_tev_set_swap(&p, 0, WL_TEV_SWAP_B, WL_TEV_SWAP_G, WL_TEV_SWAP_R, WL_TEV_SWAP_A); // table 0 is used
    gx_stub_reset();
    g_gx_stub.swap[0][0] = 99; // the caller's
    ASSERT_EQ(WL_OK, wl_tev_apply_ex(&p, WL_TEV_APPLY_SKIP_SWAPS));
    ASSERT_EQ(99, g_gx_stub.swap[0][0]);
    ASSERT_EQ(1, g_gx_stub.num_stages); // the stages went out
    ASSERT_EQ(GX_CC_TEXC, g_gx_stub.stage[0].cin[1]);
    ASSERT_EQ(0, g_gx_stub.stage[0].tex_swap);
    wl_tev_apply_unchecked_ex(&p, WL_TEV_APPLY_SKIP_SWAPS);
    ASSERT_EQ(99, g_gx_stub.swap[0][0]);
    wl_tev_apply_unchecked_ex(&p, 0); // without the flag the table is written
    ASSERT_EQ(WL_TEV_SWAP_B, g_gx_stub.swap[0][0]);
    g_gx_stub.swap[0][0] = 99;
    ASSERT_EQ(WL_OK, wl_tev_apply_ex(&p, 0));
    ASSERT_EQ(WL_TEV_SWAP_B, g_gx_stub.swap[0][0]);
    // an invalid program is refused whatever the flags
    p.stages[0].color.in[1] = 77;
    gx_stub_reset();
    ASSERT_EQ(WL_ERR_ARG, wl_tev_apply_ex(&p, WL_TEV_APPLY_SKIP_SWAPS));
    ASSERT_EQ(0u, g_gx_stub.calls);
    PASS();
}

// WL_TEV_APPLY_CHANGES: a program GX holds sends nothing, one field changed sends its one call, and
// whatever the order the programs come in, GX ends as a full apply of the last leaves it
TEST changes_send_only_what_gx_does_not_hold(void) {
    static const WlGxRecipe recipes[] = {WL_GX_RECIPE_TEX_MODULATE, WL_GX_RECIPE_VERTEX_COLOR,
                                         WL_GX_RECIPE_TEX_REPLACE, WL_GX_RECIPE_TEX_MODULATE};
    WlTevProgram p, q;
    GxStub full;
    unsigned i;

    wl_tev_recipe(&p, WL_GX_RECIPE_TEX_MODULATE);
    gx_stub_reset();
    wl_gx_shadow_forget();
    wl_tev_apply_unchecked_ex(&p, WL_TEV_APPLY_CHANGES);
    ASSERT(g_gx_stub.calls > 0u);
    g_gx_stub.calls = 0;
    wl_tev_apply_unchecked_ex(&p, WL_TEV_APPLY_CHANGES);
    ASSERT_EQ(0u, g_gx_stub.calls);
    q = p;
    q.stages[0].color.in[0] = WL_TEV_CC_HALF;
    wl_tev_apply_unchecked_ex(&q, WL_TEV_APPLY_CHANGES);
    ASSERT_EQ(1u, g_gx_stub.calls);
    ASSERT_EQ(GX_CC_HALF, g_gx_stub.stage[0].cin[0]);
    // (forgotten: everything again)
    g_gx_stub.calls = 0;
    wl_gx_shadow_forget();
    wl_tev_apply_unchecked_ex(&q, WL_TEV_APPLY_CHANGES);
    ASSERT(g_gx_stub.calls > 1u);
    // a sequence of programs: GX as the last one's full apply left it
    for (i = 0; i < sizeof recipes / sizeof recipes[0]; i++) {
        wl_tev_recipe(&p, recipes[i]);
        p.stages[0].kcsel = (uint8_t)(WL_TEV_KSEL_1 + (i & 1u));
        wl_tev_apply_unchecked_ex(&p, WL_TEV_APPLY_CHANGES);
    }
    memcpy(&full, &g_gx_stub, sizeof full);
    wl_tev_apply_unchecked(&p);
    full.calls = g_gx_stub.calls;
    ASSERT_MEM_EQ(&full, &g_gx_stub, sizeof full);
    PASS();
}

SUITE(tev_gx_suite) {
    RUN_TEST(recipes_leave_gx_as_the_hand_written_code_did);
    RUN_TEST(apply_writes_every_field);
    RUN_TEST(apply_refuses_an_invalid_program);
    RUN_TEST(unused_swap_tables_are_left_alone);
    RUN_TEST(skip_swaps_leaves_the_tables_to_the_caller);
    RUN_TEST(changes_send_only_what_gx_does_not_hold);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(tev_gx_suite);
    GREATEST_MAIN_END();
}
